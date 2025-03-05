#include <optional>

#include <Common/ProfileEvents.h>
#include <Common/FailPoint.h>
#include <Core/BackgroundSchedulePool.h>
#include <Core/Settings.h>
#include <Core/ServerSettings.h>
#include <IO/CompressionMethod.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterInsertQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/formatAST.h>
#include <Processors/Executors/CompletedPipelineExecutor.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/ISource.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/SourceStepWithFilter.h>
#include <Processors/Sources/NullSource.h>
#include <Formats/FormatFactory.h>
#include <Storages/ObjectStorageQueue/ObjectStorageQueueTableMetadata.h>
#include <Storages/ObjectStorageQueue/StorageObjectStorageQueue.h>
#include <Storages/ObjectStorageQueue/ObjectStorageQueueMetadata.h>
#include <Storages/ObjectStorageQueue/ObjectStorageQueueMetadataFactory.h>
#include <Storages/ObjectStorageQueue/ObjectStorageQueueSettings.h>
#include <Storages/StorageMaterializedView.h>
#include <Storages/StorageSnapshot.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/prepareReadingFromFormat.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Storages/AlterCommands.h>
#include <QueryPipeline/QueryPipelineBuilder.h>

#include <filesystem>

#include <fmt/ranges.h>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ObjectStorageQueueCommitRequests;
    extern const Event ObjectStorageQueueSuccessfulCommits;
    extern const Event ObjectStorageQueueUnsuccessfulCommits;
    extern const Event ObjectStorageQueueRemovedObjects;
    extern const Event ObjectStorageQueueInsertIterations;
    extern const Event ObjectStorageQueueProcessedRows;
}


namespace DB
{
namespace Setting
{
    extern const SettingsString s3queue_default_zookeeper_path;
    extern const SettingsBool s3queue_enable_logging_to_s3queue_log;
    extern const SettingsBool stream_like_engine_allow_direct_select;
    extern const SettingsBool use_concurrency_control;
}

namespace FailPoints
{
    extern const char object_storage_queue_fail_commit[];
}

namespace ServerSetting
{
    extern const ServerSettingsUInt64 keeper_multiread_batch_size;
}

namespace ObjectStorageQueueSetting
{
    extern const ObjectStorageQueueSettingsUInt32 cleanup_interval_max_ms;
    extern const ObjectStorageQueueSettingsUInt32 cleanup_interval_min_ms;
    extern const ObjectStorageQueueSettingsUInt32 enable_logging_to_queue_log;
    extern const ObjectStorageQueueSettingsString keeper_path;
    extern const ObjectStorageQueueSettingsObjectStorageQueueMode mode;
    extern const ObjectStorageQueueSettingsUInt64 max_processed_bytes_before_commit;
    extern const ObjectStorageQueueSettingsUInt64 max_processed_files_before_commit;
    extern const ObjectStorageQueueSettingsUInt64 max_processed_rows_before_commit;
    extern const ObjectStorageQueueSettingsUInt64 max_processing_time_sec_before_commit;
    extern const ObjectStorageQueueSettingsUInt64 polling_min_timeout_ms;
    extern const ObjectStorageQueueSettingsUInt64 polling_max_timeout_ms;
    extern const ObjectStorageQueueSettingsUInt64 polling_backoff_ms;
    extern const ObjectStorageQueueSettingsUInt64 processing_threads_num;
    extern const ObjectStorageQueueSettingsUInt64 buckets;
    extern const ObjectStorageQueueSettingsUInt64 tracked_file_ttl_sec;
    extern const ObjectStorageQueueSettingsUInt64 tracked_files_limit;
    extern const ObjectStorageQueueSettingsString last_processed_path;
    extern const ObjectStorageQueueSettingsUInt64 loading_retries;
    extern const ObjectStorageQueueSettingsObjectStorageQueueAction after_processing;
    extern const ObjectStorageQueueSettingsUInt64 list_objects_batch_size;
    extern const ObjectStorageQueueSettingsBool enable_hash_ring_filtering;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int BAD_QUERY_PARAMETER;
    extern const int QUERY_NOT_ALLOWED;
    extern const int SUPPORT_IS_DISABLED;
    extern const int UNKNOWN_EXCEPTION;
}

namespace
{
    std::string chooseZooKeeperPath(const StorageID & table_id, const Settings & settings, const ObjectStorageQueueSettings & queue_settings)
    {
        std::string zk_path_prefix = settings[Setting::s3queue_default_zookeeper_path].value;
        if (zk_path_prefix.empty())
            zk_path_prefix = "/";

        std::string result_zk_path;
        if (queue_settings[ObjectStorageQueueSetting::keeper_path].changed)
        {
            /// We do not add table uuid here on purpose.
            result_zk_path = fs::path(zk_path_prefix) / queue_settings[ObjectStorageQueueSetting::keeper_path].value;
        }
        else
        {
            auto database_uuid = DatabaseCatalog::instance().getDatabase(table_id.database_name)->getUUID();
            result_zk_path = fs::path(zk_path_prefix) / toString(database_uuid) / toString(table_id.uuid);
        }
        return zkutil::extractZooKeeperPath(result_zk_path, true);
    }

    void validateSettings(
        ObjectStorageQueueSettings & queue_settings,
        bool is_attach)
    {
        if (!is_attach && !queue_settings[ObjectStorageQueueSetting::mode].changed)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Setting `mode` (Unordered/Ordered) is not specified, but is required.");
        }
        /// In case !is_attach, we leave Ordered mode as default for compatibility.

        if (!queue_settings[ObjectStorageQueueSetting::processing_threads_num])
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Setting `processing_threads_num` cannot be set to zero");
        }

        if (queue_settings[ObjectStorageQueueSetting::cleanup_interval_min_ms] > queue_settings[ObjectStorageQueueSetting::cleanup_interval_max_ms])
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                            "Setting `cleanup_interval_min_ms` ({}) must be less or equal to `cleanup_interval_max_ms` ({})",
                            queue_settings[ObjectStorageQueueSetting::cleanup_interval_min_ms].value, queue_settings[ObjectStorageQueueSetting::cleanup_interval_max_ms].value);
        }
    }

    std::shared_ptr<ObjectStorageQueueLog> getQueueLog(
        const ObjectStoragePtr & storage,
        const ContextPtr & context,
        bool enable_logging_to_queue_log)
    {
        const auto & settings = context->getSettingsRef();
        switch (storage->getType())
        {
            case DB::ObjectStorageType::S3:
            {
                if (enable_logging_to_queue_log || settings[Setting::s3queue_enable_logging_to_s3queue_log])
                    return context->getS3QueueLog();
                return nullptr;
            }
            case DB::ObjectStorageType::Azure:
            {
                if (enable_logging_to_queue_log)
                    return context->getAzureQueueLog();
                return nullptr;
            }
            default:
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected object storage type: {}", storage->getType());
        }
    }
}

StorageObjectStorageQueue::StorageObjectStorageQueue(
    std::unique_ptr<ObjectStorageQueueSettings> queue_settings_,
    const ConfigurationPtr configuration_,
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & comment,
    ContextPtr context_,
    std::optional<FormatSettings> format_settings_,
    ASTStorage * engine_args,
    LoadingStrictnessLevel mode)
    : IStorage(table_id_)
    , WithContext(context_)
    , type(configuration_->getType())
    , engine_name(engine_args->engine->name)
    , zk_path(chooseZooKeeperPath(table_id_, context_->getSettingsRef(), *queue_settings_))
    , enable_logging_to_queue_log((*queue_settings_)[ObjectStorageQueueSetting::enable_logging_to_queue_log])
    , polling_min_timeout_ms((*queue_settings_)[ObjectStorageQueueSetting::polling_min_timeout_ms])
    , polling_max_timeout_ms((*queue_settings_)[ObjectStorageQueueSetting::polling_max_timeout_ms])
    , polling_backoff_ms((*queue_settings_)[ObjectStorageQueueSetting::polling_backoff_ms])
    , list_objects_batch_size((*queue_settings_)[ObjectStorageQueueSetting::list_objects_batch_size])
    , enable_hash_ring_filtering((*queue_settings_)[ObjectStorageQueueSetting::enable_hash_ring_filtering])
    , commit_settings(CommitSettings{
        .max_processed_files_before_commit = (*queue_settings_)[ObjectStorageQueueSetting::max_processed_files_before_commit],
        .max_processed_rows_before_commit = (*queue_settings_)[ObjectStorageQueueSetting::max_processed_rows_before_commit],
        .max_processed_bytes_before_commit = (*queue_settings_)[ObjectStorageQueueSetting::max_processed_bytes_before_commit],
        .max_processing_time_sec_before_commit = (*queue_settings_)[ObjectStorageQueueSetting::max_processing_time_sec_before_commit],
    })
    , configuration{configuration_}
    , format_settings(format_settings_)
    , reschedule_processing_interval_ms((*queue_settings_)[ObjectStorageQueueSetting::polling_min_timeout_ms])
    , log(getLogger(fmt::format("Storage{}Queue ({})", configuration->getEngineName(), table_id_.getFullTableName())))
{
    if (configuration->getPath().empty())
    {
        configuration->setPath("/*");
    }
    else if (configuration->getPath().ends_with('/'))
    {
        configuration->setPath(configuration->getPath() + '*');
    }
    else if (!configuration->isPathWithGlobs())
    {
        throw Exception(ErrorCodes::BAD_QUERY_PARAMETER, "ObjectStorageQueue url must either end with '/' or contain globs");
    }

    const bool is_attach = mode > LoadingStrictnessLevel::CREATE;
    validateSettings(*queue_settings_, is_attach);

    object_storage = configuration->createObjectStorage(context_, /* is_readonly */true);
    FormatFactory::instance().checkFormatName(configuration->format);
    configuration->check(context_);

    ColumnsDescription columns{columns_};
    std::string sample_path;
    resolveSchemaAndFormat(columns, configuration->format, object_storage, configuration, format_settings, sample_path, context_);
    configuration->check(context_);

    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns);
    storage_metadata.setConstraints(constraints_);
    storage_metadata.setComment(comment);
    if (engine_args->settings)
        storage_metadata.settings_changes = engine_args->settings->ptr();
    setVirtuals(VirtualColumnUtils::getVirtualsForFileLikeStorage(storage_metadata.columns, context_));
    setInMemoryMetadata(storage_metadata);

    LOG_INFO(log, "Using zookeeper path: {}", zk_path.string());

    auto table_metadata = ObjectStorageQueueMetadata::syncWithKeeper(
        zk_path, *queue_settings_, storage_metadata.getColumns(), configuration_->format, context_, is_attach, log);

    ObjectStorageType storage_type = engine_name == "S3Queue" ? ObjectStorageType::S3 : ObjectStorageType::Azure;

    temp_metadata = std::make_unique<ObjectStorageQueueMetadata>(
        storage_type,
        zk_path,
        std::move(table_metadata),
        (*queue_settings_)[ObjectStorageQueueSetting::cleanup_interval_min_ms],
        (*queue_settings_)[ObjectStorageQueueSetting::cleanup_interval_max_ms],
        getContext()->getServerSettings()[ServerSetting::keeper_multiread_batch_size]);

    task = getContext()->getSchedulePool().createTask("ObjectStorageQueueStreamingTask", [this] { threadFunc(); });
}

void StorageObjectStorageQueue::startup()
{
    /// Register the metadata in startup(), unregister in shutdown.
    /// (If startup is never called, shutdown also won't be called.)
    files_metadata = ObjectStorageQueueMetadataFactory::instance().getOrCreate(zk_path, std::move(temp_metadata), getStorageID());

    if (task)
        task->activateAndSchedule();
}

void StorageObjectStorageQueue::shutdown(bool is_drop)
{
    table_is_being_dropped = is_drop;
    shutdown_called = true;

    LOG_TRACE(log, "Shutting down storage...");
    if (task)
    {
        task->deactivate();
    }

    if (files_metadata)
    {
        try
        {
            files_metadata->unregister(getStorageID(), /* active */true);
        }
        catch (...)
        {
            tryLogCurrentException(log);
        }

        files_metadata->shutdown();
        files_metadata.reset();
    }
    LOG_TRACE(log, "Shut down storage");
}

void StorageObjectStorageQueue::drop()
{
    ObjectStorageQueueMetadataFactory::instance().remove(zk_path, getStorageID());
}

bool StorageObjectStorageQueue::supportsSubsetOfColumns(const ContextPtr & context_) const
{
    return FormatFactory::instance().checkIfFormatSupportsSubsetOfColumns(configuration->format, context_, format_settings);
}

class ReadFromObjectStorageQueue : public SourceStepWithFilter
{
public:
    std::string getName() const override { return "ReadFromObjectStorageQueue"; }
    void initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &) override;
    void applyFilters(ActionDAGNodes added_filter_nodes) override;

    ReadFromObjectStorageQueue(
        const Names & column_names_,
        const SelectQueryInfo & query_info_,
        const StorageSnapshotPtr & storage_snapshot_,
        const ContextPtr & context_,
        Block sample_block,
        ReadFromFormatInfo info_,
        std::shared_ptr<StorageObjectStorageQueue> storage_,
        size_t max_block_size_)
        : SourceStepWithFilter(
            std::move(sample_block),
            column_names_,
            query_info_,
            storage_snapshot_,
            context_)
        , info(std::move(info_))
        , storage(std::move(storage_))
        , max_block_size(max_block_size_)
    {
    }

private:
    ReadFromFormatInfo info;
    std::shared_ptr<StorageObjectStorageQueue> storage;
    size_t max_block_size;

    std::shared_ptr<StorageObjectStorageQueue::FileIterator> iterator;

    void createIterator(const ActionsDAG::Node * predicate);
};

void ReadFromObjectStorageQueue::createIterator(const ActionsDAG::Node * predicate)
{
    if (iterator)
        return;

    iterator = storage->createFileIterator(context, predicate);
}


void ReadFromObjectStorageQueue::applyFilters(ActionDAGNodes added_filter_nodes)
{
    SourceStepWithFilter::applyFilters(std::move(added_filter_nodes));

    const ActionsDAG::Node * predicate = nullptr;
    if (filter_actions_dag)
        predicate = filter_actions_dag->getOutputs().at(0);

    createIterator(predicate);
}

void StorageObjectStorageQueue::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    size_t)
{
    if (!local_context->getSettingsRef()[Setting::stream_like_engine_allow_direct_select])
    {
        throw Exception(ErrorCodes::QUERY_NOT_ALLOWED, "Direct select is not allowed. "
                        "To enable use setting `stream_like_engine_allow_direct_select`");
    }

    if (mv_attached)
    {
        throw Exception(ErrorCodes::QUERY_NOT_ALLOWED,
                        "Cannot read from {} with attached materialized views", getName());
    }

    auto this_ptr = std::static_pointer_cast<StorageObjectStorageQueue>(shared_from_this());
    auto read_from_format_info = prepareReadingFromFormat(column_names, storage_snapshot, local_context, supportsSubsetOfColumns(local_context));

    auto reading = std::make_unique<ReadFromObjectStorageQueue>(
        column_names,
        query_info,
        storage_snapshot,
        local_context,
        read_from_format_info.source_header,
        read_from_format_info,
        std::move(this_ptr),
        max_block_size);

    query_plan.addStep(std::move(reading));
}

void ReadFromObjectStorageQueue::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    Pipes pipes;

    size_t processing_threads_num = storage->getTableMetadata().processing_threads_num;

    createIterator(nullptr);
    auto progress = std::make_shared<ObjectStorageQueueSource::ProcessingProgress>();
    for (size_t i = 0; i < processing_threads_num; ++i)
        pipes.emplace_back(storage->createSource(
                               i/* processor_id */,
                               info,
                               progress,
                               iterator,
                               max_block_size,
                               context,
                               true/* commit_once_processed */));

    auto pipe = Pipe::unitePipes(std::move(pipes));
    if (pipe.empty())
        pipe = Pipe(std::make_shared<NullSource>(info.source_header));

    for (const auto & processor : pipe.getProcessors())
        processors.emplace_back(processor);

    pipeline.init(std::move(pipe));
}

std::shared_ptr<ObjectStorageQueueSource> StorageObjectStorageQueue::createSource(
    size_t processor_id,
    const ReadFromFormatInfo & info,
    ProcessingProgressPtr progress_,
    std::shared_ptr<StorageObjectStorageQueue::FileIterator> file_iterator,
    size_t max_block_size,
    ContextPtr local_context,
    bool commit_once_processed)
{
    CommitSettings commit_settings_copy;
    {
        std::lock_guard lock(mutex);
        commit_settings_copy = commit_settings;
    }
    return std::make_shared<ObjectStorageQueueSource>(
        getName(), processor_id,
        file_iterator, configuration, object_storage, progress_,
        info, format_settings,
        commit_settings_copy,
        files_metadata,
        local_context, max_block_size, shutdown_called, table_is_being_dropped,
        getQueueLog(object_storage, local_context, enable_logging_to_queue_log),
        getStorageID(), log, commit_once_processed);
}

size_t StorageObjectStorageQueue::getDependencies() const
{
    auto table_id = getStorageID();

    // Check if all dependencies are attached
    auto view_ids = DatabaseCatalog::instance().getDependentViews(table_id);
    LOG_TEST(log, "Number of attached views {} for {}", view_ids.size(), table_id.getNameForLogs());

    if (view_ids.empty())
        return 0;

    // Check the dependencies are ready?
    for (const auto & view_id : view_ids)
    {
        auto view = DatabaseCatalog::instance().tryGetTable(view_id, getContext());
        if (!view)
            return 0;

        // If it materialized view, check it's target table
        auto * materialized_view = dynamic_cast<StorageMaterializedView *>(view.get());
        if (materialized_view && !materialized_view->tryGetTargetTable())
            return 0;
    }

    return view_ids.size();
}

void StorageObjectStorageQueue::threadFunc()
{
    if (shutdown_called)
        return;

    const auto storage_id = getStorageID();
    try
    {
        const size_t dependencies_count = getDependencies();
        if (dependencies_count)
        {
            mv_attached.store(true);
            SCOPE_EXIT({ mv_attached.store(false); });

            LOG_DEBUG(log, "Started streaming to {} attached views", dependencies_count);

            files_metadata->registerIfNot(storage_id, /* active */true);

            if (streamToViews())
            {
                /// Reset the reschedule interval.
                std::lock_guard lock(mutex);
                reschedule_processing_interval_ms = polling_min_timeout_ms;
            }
            else
            {
                /// Increase the reschedule interval.
                std::lock_guard lock(mutex);
                reschedule_processing_interval_ms = std::min<size_t>(polling_max_timeout_ms, reschedule_processing_interval_ms + polling_backoff_ms);
            }

            LOG_DEBUG(log, "Stopped streaming to {} attached views", dependencies_count);
        }
        else
        {
            LOG_TEST(log, "No attached dependencies");
        }
    }
    catch (...)
    {
        LOG_ERROR(log, "Failed to process data: {}", getCurrentExceptionMessage(true));
    }

    if (!shutdown_called)
    {
        LOG_TRACE(log, "Reschedule processing thread in {} ms", reschedule_processing_interval_ms);
        task->scheduleAfter(reschedule_processing_interval_ms);

        if (reschedule_processing_interval_ms > 5000) /// TODO: Add a setting
        {
            try
            {
                files_metadata->unregister(storage_id, /* active */true);
            }
            catch (...)
            {
                tryLogCurrentException(log);
            }
        }
    }
}

bool StorageObjectStorageQueue::streamToViews()
{
    // Create a stream for each consumer and join them in a union stream
    // Only insert into dependent views and expect that input blocks contain virtual columns

    auto table_id = getStorageID();
    auto table = DatabaseCatalog::instance().getTable(table_id, getContext());
    if (!table)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Engine table {} doesn't exist.", table_id.getNameForLogs());

    auto insert = std::make_shared<ASTInsertQuery>();
    insert->table_id = table_id;

    auto storage_snapshot = getStorageSnapshot(getInMemoryMetadataPtr(), getContext());
    auto queue_context = Context::createCopy(getContext());
    queue_context->makeQueryContext();

    auto file_iterator = createFileIterator(queue_context, nullptr);
    size_t total_rows = 0;
    const size_t processing_threads_num = getTableMetadata().processing_threads_num;

    LOG_TEST(log, "Using {} processing threads", processing_threads_num);

    while (!shutdown_called && !file_iterator->isFinished())
    {
        /// FIXME:
        /// it is possible that MV is dropped just before we start the insert,
        /// but in this case we would not throw any exception, so
        /// data will not be inserted anywhere.
        InterpreterInsertQuery interpreter(
            insert,
            queue_context,
            /* allow_materialized */ false,
            /* no_squash */ true,
            /* no_destination */ true,
            /* async_isnert */ false);
        auto block_io = interpreter.execute();
        auto read_from_format_info = prepareReadingFromFormat(
            block_io.pipeline.getHeader().getNames(),
            storage_snapshot,
            queue_context,
            supportsSubsetOfColumns(queue_context));

        Pipes pipes;
        std::vector<std::shared_ptr<ObjectStorageQueueSource>> sources;

        pipes.reserve(processing_threads_num);
        sources.reserve(processing_threads_num);

        auto processing_progress = std::make_shared<ProcessingProgress>();
        for (size_t i = 0; i < processing_threads_num; ++i)
        {
            auto source = createSource(
                i/* processor_id */,
                read_from_format_info,
                processing_progress,
                file_iterator,
                DBMS_DEFAULT_BUFFER_SIZE,
                queue_context,
                false/* commit_once_processed */);

            pipes.emplace_back(source);
            sources.emplace_back(source);
        }
        auto pipe = Pipe::unitePipes(std::move(pipes));

        block_io.pipeline.complete(std::move(pipe));
        block_io.pipeline.setNumThreads(processing_threads_num);
        block_io.pipeline.setConcurrencyControl(queue_context->getSettingsRef()[Setting::use_concurrency_control]);

        std::atomic_size_t rows = 0;
        block_io.pipeline.setProgressCallback([&](const Progress & progress) { rows += progress.read_rows.load(); });

        ProfileEvents::increment(ProfileEvents::ObjectStorageQueueInsertIterations);

        try
        {
            CompletedPipelineExecutor executor(block_io.pipeline);
            executor.execute();
        }
        catch (...)
        {
            commit(/* insert_succeeded */false, rows, sources, getCurrentExceptionMessage(true));
            file_iterator->releaseFinishedBuckets();
            throw;
        }

        commit(/* insert_succeeded */true, rows, sources);
        file_iterator->releaseFinishedBuckets();
        total_rows += rows;
    }

    LOG_TEST(log, "Processed rows: {}", total_rows);
    return total_rows > 0;
}

void StorageObjectStorageQueue::commit(
    bool insert_succeeded,
    size_t inserted_rows,
    std::vector<std::shared_ptr<ObjectStorageQueueSource>> & sources,
    const std::string & exception_message) const
{
    ProfileEvents::increment(ProfileEvents::ObjectStorageQueueProcessedRows, inserted_rows);

    Coordination::Requests requests;
    StoredObjects successful_objects;
    for (auto & source : sources)
        source->prepareCommitRequests(requests, insert_succeeded, successful_objects, exception_message);

    if (requests.empty())
    {
        LOG_TEST(log, "Nothing to commit");
        return;
    }

    ProfileEvents::increment(ProfileEvents::ObjectStorageQueueCommitRequests, requests.size());

    if (!successful_objects.empty()
        && files_metadata->getTableMetadata().after_processing == ObjectStorageQueueAction::DELETE)
    {
        /// We do need to apply after-processing action before committing requests to keeper.
        /// See explanation in ObjectStorageQueueSource::FileIterator::nextImpl().
        object_storage->removeObjectsIfExist(successful_objects);
        ProfileEvents::increment(ProfileEvents::ObjectStorageQueueRemovedObjects, successful_objects.size());
    }

    auto zk_client = getZooKeeper();
    Coordination::Responses responses;

    fiu_do_on(FailPoints::object_storage_queue_fail_commit, {
        throw Exception(ErrorCodes::UNKNOWN_EXCEPTION, "Failed to commit processed files");
    });

    auto code = zk_client->tryMulti(requests, responses);
    if (code != Coordination::Error::ZOK)
    {
        ProfileEvents::increment(ProfileEvents::ObjectStorageQueueUnsuccessfulCommits);
        throw zkutil::KeeperMultiException(code, requests, responses);
    }

    ProfileEvents::increment(ProfileEvents::ObjectStorageQueueSuccessfulCommits);

    for (auto & source : sources)
        source->finalizeCommit(insert_succeeded, exception_message);

    LOG_TRACE(
        log, "Successfully committed {} requests for {} sources (inserted rows: {}, successful files: {})",
        requests.size(), sources.size(), inserted_rows, successful_objects.size());
}

static const std::unordered_set<std::string_view> changeable_settings_unordered_mode
{
    "processing_threads_num",
    "loading_retries",
    "after_processing",
    "tracked_files_limit",
    "tracked_file_ttl_sec",
    "polling_min_timeout_ms",
    "polling_max_timeout_ms",
    "polling_backoff_ms",
    "max_processed_files_before_commit",
    "max_processed_rows_before_commit",
    "max_processed_bytes_before_commit",
    "max_processing_time_sec_before_commit",
    "enable_hash_ring_filtering",
    "list_objects_batch_size",
};

static const std::unordered_set<std::string_view> changeable_settings_ordered_mode
{
    "loading_retries",
    "after_processing",
    "polling_min_timeout_ms",
    "polling_max_timeout_ms",
    "polling_backoff_ms",
    "max_processed_files_before_commit",
    "max_processed_rows_before_commit",
    "max_processed_bytes_before_commit",
    "max_processing_time_sec_before_commit",
    "buckets",
    "list_objects_batch_size",
};

static std::string normalizeSetting(const std::string & name)
{
    /// We support this prefix for compatibility.
    if (name.starts_with("s3queue_"))
        return name.substr(std::strlen("s3queue_"));
    return name;
}

void checkNormalizedSetting(const std::string & name)
{
    if (name.starts_with("s3queue_"))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Setting is not normalized: {}", name);
}

static bool isSettingChangeable(const std::string & name, ObjectStorageQueueMode mode)
{
    checkNormalizedSetting(name);

    if (mode == ObjectStorageQueueMode::UNORDERED)
        return changeable_settings_unordered_mode.contains(name);
    else
        return changeable_settings_ordered_mode.contains(name);
}

static bool requiresDetachedMV(const std::string & name)
{
    checkNormalizedSetting(name);
    return name == "buckets";
}

static AlterCommands normalizeAlterCommands(const AlterCommands & alter_commands)
{
    /// Remove s3queue_ prefix from setting to avoid duplicated settings,
    /// because of altering setting with the prefix to a setting without the prefix.
    AlterCommands normalized_alter_commands(alter_commands);
    for (auto & command : normalized_alter_commands)
    {
        for (auto & setting : command.settings_changes)
            setting.name = normalizeSetting(setting.name);

        std::set<std::string> settings_resets;
        for (const auto & setting : command.settings_resets)
            settings_resets.insert(normalizeSetting(setting));

        command.settings_resets = settings_resets;
    }
    return normalized_alter_commands;
}

void StorageObjectStorageQueue::checkAlterIsPossible(const AlterCommands & commands, ContextPtr local_context) const
{
    for (const auto & command : commands)
    {
        if (command.type != AlterCommand::MODIFY_SETTING && command.type != AlterCommand::RESET_SETTING)
        {
            throw Exception(
                ErrorCodes::SUPPORT_IS_DISABLED,
                "Only MODIFY/RESET SETTING alter is allowed for {}", getName());
        }
    }

    StorageInMemoryMetadata old_metadata(getInMemoryMetadata());
    SettingsChanges * old_settings = nullptr;
    if (old_metadata.settings_changes)
    {
        old_settings = &old_metadata.settings_changes->as<ASTSetQuery &>().changes;
        for (auto & setting : *old_settings)
            setting.name = normalizeSetting(setting.name);
    }

    StorageInMemoryMetadata new_metadata(old_metadata);

    auto alter_commands = normalizeAlterCommands(commands);
    alter_commands.apply(new_metadata, local_context);

    if (!new_metadata.hasSettingsChanges())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "No settings changes");

    const auto mode = getTableMetadata().getMode();
    const auto & new_settings = new_metadata.settings_changes->as<ASTSetQuery &>().changes;

    for (const auto & setting : new_settings)
    {
        bool setting_changed = true;
        if (old_settings)
        {
            auto it = std::find_if(
                old_settings->begin(), old_settings->end(),
                [&](const SettingChange & change) { return change.name == setting.name; });

            setting_changed = it != old_settings->end() && it->value != setting.value;
        }

        if (setting_changed)
        {
            /// `new_settings` contains a full set of settings, changed and non-changed together.
            /// So we check whether setting is allowed to be changed only if it is actually changed.
            if (!isSettingChangeable(setting.name, mode))
            {
                throw Exception(
                    ErrorCodes::SUPPORT_IS_DISABLED,
                    "Changing setting {} is not allowed for {} mode of {}",
                    setting.name, magic_enum::enum_name(mode), getName());
            }

            /// Some settings affect the work of background processing thread,
            /// so might require its cancellation.
            if (requiresDetachedMV(setting.name))
            {
                const size_t dependencies_count = getDependencies();
                if (dependencies_count)
                {
                    throw Exception(
                        ErrorCodes::SUPPORT_IS_DISABLED,
                        "Changing setting {} is allowed "
                        "only with detached dependencies (dependencies count: {})",
                        setting.name, dependencies_count);
                }
            }
        }
    }
}

void StorageObjectStorageQueue::alter(
    const AlterCommands & commands,
    ContextPtr local_context,
    AlterLockHolder &)
{
    if (commands.isSettingsAlter())
    {
        auto table_id = getStorageID();
        auto alter_commands = normalizeAlterCommands(commands);

        StorageInMemoryMetadata old_metadata(getInMemoryMetadata());
        SettingsChanges * old_settings = nullptr;
        if (old_metadata.settings_changes)
        {
            old_settings = &old_metadata.settings_changes->as<ASTSetQuery &>().changes;
            for (auto & setting : *old_settings)
                setting.name = normalizeSetting(setting.name);
        }

        /// settings_changes will be cloned.
        StorageInMemoryMetadata new_metadata(old_metadata);
        alter_commands.apply(new_metadata, local_context);
        auto & new_settings = new_metadata.settings_changes->as<ASTSetQuery &>().changes;

        if (old_settings)
        {
            auto get_names = [](const SettingsChanges & settings)
            {
                std::set<std::string> names;
                for (const auto & [name, _] : settings)
                {
                    auto inserted = names.insert(name).second;
                    if (!inserted)
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Setting {} is duplicated", name);
                }
                return names;
            };

            auto old_settings_set = get_names(*old_settings);
            auto new_settings_set = get_names(new_settings);

            std::set<std::string> reset_settings;
            std::set_difference(
                old_settings_set.begin(), old_settings_set.end(),
                new_settings_set.begin(), new_settings_set.end(),
                std::inserter(reset_settings, reset_settings.begin()));

            if (!reset_settings.empty())
            {
                LOG_TRACE(
                    log, "Will reset settings: {} (old settings: {}, new_settings: {})",
                    fmt::join(reset_settings, ", "),
                    fmt::join(old_settings_set, ", "), fmt::join(new_settings_set, ", "));

                ObjectStorageQueueSettings default_settings;
                for (const auto & name : reset_settings)
                    new_settings.push_back(SettingChange(name, default_settings.get(name)));
            }
        }

        SettingsChanges changed_settings;
        std::set<std::string> new_settings_set;

        const auto mode = getTableMetadata().getMode();
        for (auto & setting : new_settings)
        {
            LOG_TEST(log, "New setting {}: {}", setting.name, setting.value);

            auto inserted = new_settings_set.emplace(setting.name).second;
            if (!inserted)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Setting {} is duplicated", setting.name);

            bool setting_changed = true;
            if (old_settings)
            {
                auto it = std::find_if(
                    old_settings->begin(), old_settings->end(),
                    [&](const SettingChange & change) { return change.name == setting.name; });

                setting_changed = it == old_settings->end() || it->value != setting.value;
            }
            if (!setting_changed)
                continue;

            if (!isSettingChangeable(setting.name, mode))
            {
                throw Exception(
                    ErrorCodes::SUPPORT_IS_DISABLED,
                    "Changing setting {} is not allowed for {} mode of {}",
                    setting.name, magic_enum::enum_name(mode), getName());
            }

            if (requiresDetachedMV(setting.name))
            {
                const size_t dependencies_count = getDependencies();
                if (dependencies_count)
                {
                    throw Exception(
                        ErrorCodes::SUPPORT_IS_DISABLED,
                        "Changing setting {} is not allowed only with detached dependencies "
                        "(dependencies count: {})",
                        setting.name, dependencies_count);
                }
            }

            changed_settings.push_back(setting);
        }

        LOG_TEST(log, "New settings: {}", serializeAST(*new_metadata.settings_changes));

        /// Alter settings which are stored in keeper.
        files_metadata->alterSettings(changed_settings, local_context);

        /// Alter settings which are not stored in keeper.
        for (const auto & change : changed_settings)
        {
            std::lock_guard lock(mutex);

            if (change.name == "polling_min_timeout_ms")
                polling_min_timeout_ms = change.value.safeGet<UInt64>();
            if (change.name == "polling_max_timeout_ms")
                polling_max_timeout_ms = change.value.safeGet<UInt64>();
            if (change.name == "polling_backoff_ms")
                polling_backoff_ms = change.value.safeGet<UInt64>();

            if (change.name == "max_processed_files_before_commit")
                commit_settings.max_processed_files_before_commit = change.value.safeGet<UInt64>();
            if (change.name == "max_processed_rows_before_commit")
                commit_settings.max_processed_rows_before_commit = change.value.safeGet<UInt64>();
            if (change.name == "max_processed_bytes_before_commit")
                commit_settings.max_processed_bytes_before_commit = change.value.safeGet<UInt64>();
            if (change.name == "max_processing_time_sec_before_commit")
                commit_settings.max_processing_time_sec_before_commit = change.value.safeGet<UInt64>();

            if (change.name == "list_objects_batch_size")
                list_objects_batch_size = change.value.safeGet<UInt64>();
            if (change.name == "enable_hash_ring_filtering")
                enable_hash_ring_filtering = change.value.safeGet<bool>();
        }

        DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(local_context, table_id, new_metadata);
        setInMemoryMetadata(new_metadata);
    }
}

zkutil::ZooKeeperPtr StorageObjectStorageQueue::getZooKeeper() const
{
    return getContext()->getZooKeeper();
}

std::shared_ptr<StorageObjectStorageQueue::FileIterator>
StorageObjectStorageQueue::createFileIterator(ContextPtr local_context, const ActionsDAG::Node * predicate)
{
    const auto & table_metadata = getTableMetadata();
    bool file_deletion_enabled = table_metadata.getMode() == ObjectStorageQueueMode::UNORDERED
        && (table_metadata.tracked_files_ttl_sec || table_metadata.tracked_files_limit);

    size_t list_objects_batch_size_copy;
    bool enable_hash_ring_filtering_copy;
    {
        std::lock_guard lock(mutex);
        list_objects_batch_size_copy = list_objects_batch_size;
        enable_hash_ring_filtering_copy = enable_hash_ring_filtering;
    }

    return std::make_shared<FileIterator>(
        files_metadata,
        object_storage,
        configuration,
        getStorageID(),
        list_objects_batch_size_copy,
        predicate,
        getVirtualsList(),
        local_context,
        log,
        enable_hash_ring_filtering_copy,
        file_deletion_enabled,
        shutdown_called);
}

ObjectStorageQueueSettings StorageObjectStorageQueue::getSettings() const
{
    /// We do not store queue settings
    /// (because of the inconvenience of keeping them in sync with ObjectStorageQueueTableMetadata),
    /// so let's reconstruct.
    ObjectStorageQueueSettings settings;
    const auto & table_metadata = getTableMetadata();
    settings[ObjectStorageQueueSetting::mode] = table_metadata.mode;
    settings[ObjectStorageQueueSetting::after_processing] = table_metadata.after_processing;
    settings[ObjectStorageQueueSetting::keeper_path] = zk_path;
    settings[ObjectStorageQueueSetting::loading_retries] = table_metadata.loading_retries;
    settings[ObjectStorageQueueSetting::processing_threads_num] = table_metadata.processing_threads_num;
    settings[ObjectStorageQueueSetting::enable_logging_to_queue_log] = enable_logging_to_queue_log;
    settings[ObjectStorageQueueSetting::last_processed_path] = table_metadata.last_processed_path;
    settings[ObjectStorageQueueSetting::tracked_file_ttl_sec] = table_metadata.tracked_files_ttl_sec;
    settings[ObjectStorageQueueSetting::tracked_files_limit] = table_metadata.tracked_files_limit;
    settings[ObjectStorageQueueSetting::cleanup_interval_min_ms] = 0;
    settings[ObjectStorageQueueSetting::cleanup_interval_max_ms] = 0;
    settings[ObjectStorageQueueSetting::buckets] = table_metadata.buckets;

    {
        std::lock_guard lock(mutex);
        settings[ObjectStorageQueueSetting::polling_min_timeout_ms] = polling_min_timeout_ms;
        settings[ObjectStorageQueueSetting::polling_max_timeout_ms] = polling_max_timeout_ms;
        settings[ObjectStorageQueueSetting::polling_backoff_ms] = polling_backoff_ms;
        settings[ObjectStorageQueueSetting::max_processed_files_before_commit] = commit_settings.max_processed_files_before_commit;
        settings[ObjectStorageQueueSetting::max_processed_rows_before_commit] = commit_settings.max_processed_rows_before_commit;
        settings[ObjectStorageQueueSetting::max_processed_bytes_before_commit] = commit_settings.max_processed_bytes_before_commit;
        settings[ObjectStorageQueueSetting::max_processing_time_sec_before_commit] = commit_settings.max_processing_time_sec_before_commit;
        settings[ObjectStorageQueueSetting::enable_hash_ring_filtering] = enable_hash_ring_filtering;
        settings[ObjectStorageQueueSetting::list_objects_batch_size] = list_objects_batch_size;
    }

    return settings;
}

}
