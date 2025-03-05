#pragma once
#include <Processors/QueryPlan/ITransformingStep.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Cache/QueryConditionCache.h>

namespace DB
{

/// Implements WHERE, HAVING operations. See FilterTransform.
class FilterStep : public ITransformingStep
{
public:
    FilterStep(
        const Header & input_header_,
        ActionsDAG actions_dag_,
        String filter_column_name_,
        bool remove_filter_column_);

    String getName() const override { return "Filter"; }
    void transformPipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings & settings) override;

    void describeActions(JSONBuilder::JSONMap & map) const override;
    void describeActions(FormatSettings & settings) const override;

    const ActionsDAG & getExpression() const { return actions_dag; }
    ActionsDAG & getExpression() { return actions_dag; }
    const String & getFilterColumnName() const { return filter_column_name; }
    bool removesFilterColumn() const { return remove_filter_column; }
    void setQueryConditionKey(size_t condition_hash_);

    static bool canUseType(const DataTypePtr & type);

    void serialize(Serialization & ctx) const override;

    static std::unique_ptr<IQueryPlanStep> deserialize(Deserialization & ctx);

private:
    void updateOutputHeader() override;

    ActionsDAG actions_dag;
    String filter_column_name;
    bool remove_filter_column;

    std::optional<size_t> condition_hash;
};

}
