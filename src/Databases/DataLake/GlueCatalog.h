#pragma once
#include "config.h"

#if USE_AWS_S3 && USE_AVRO

#include <aws/core/auth/AWSCredentials.h>
#include <Databases/DataLake/ICatalog.h>
#include <Interpreters/Context_fwd.h>
#include <Poco/JSON/Object.h>
#include <aws/glue/GlueClient.h>

namespace DataLake
{

class GlueCatalog final : public ICatalog, private DB::WithContext
{
public:
    GlueCatalog(
        const String & access_key_id,
        const String & secret_access_key,
        const String & region,
        const String & endpoint,
        DB::ContextPtr context_);

    ~GlueCatalog() override = default;

    bool empty() const override;

    DB::Names getTables() const override;

    bool existsTable(const std::string & schema_name, const std::string & table_name) const override;

    void getTableMetadata(
        const std::string & schema_name,
        const std::string & table_name,
        TableMetadata & result) const override;

    bool tryGetTableMetadata(
        const std::string & schema_name,
        const std::string & table_name,
        TableMetadata & result) const override;

    std::optional<StorageType> getStorageType() const override
    {
        return StorageType::S3;
    }

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::GLUE;
    }


    DB::StorageObjectStorageSettingsPtr createStorageSettingsFromMetadata(const TableMetadata & metadata) const override;
private:
    std::unique_ptr<Aws::Glue::GlueClient> glue_client;
    const LoggerPtr log;
    Aws::Auth::AWSCredentials credentials;
    std::string region;

    DataLake::ICatalog::Namespaces getDatabases(const std::string & prefix, size_t limit = 0) const;
    DB::Names getTablesForDatabase(const std::string & db_name, size_t limit = 0) const;
    void setCredentials(TableMetadata & metadata) const;
};

}

#endif
