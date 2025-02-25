#include <Storages/ColumnsDescription.h>
#include <Storages/System/StorageSystemPartsBase.h>
#include <Common/escapeForFileName.h>
#include <Columns/ColumnString.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDate.h>
#include <DataStreams/OneBlockInputStream.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/VirtualColumnUtils.h>
#include <Databases/IDatabase.h>
#include <Parsers/queryToString.h>
#include <Parsers/ASTIdentifier.h>


namespace DB
{

bool StorageSystemPartsBase::hasStateColumn(const Names & column_names) const
{
    bool has_state_column = false;
    Names real_column_names;

    for (const String & column_name : column_names)
    {
        if (column_name == "_state")
            has_state_column = true;
        else
            real_column_names.emplace_back(column_name);
    }

    /// Do not check if only _state column is requested
    if (!(has_state_column && real_column_names.empty()))
        check(real_column_names);

    return has_state_column;
}

MergeTreeData::DataPartsVector
StoragesInfo::getParts(MergeTreeData::DataPartStateVector & state, bool has_state_column) const
{
    using State = MergeTreeData::DataPartState;
    if (need_inactive_parts)
    {
        /// If has_state_column is requested, return all states.
        if (!has_state_column)
            return data->getDataPartsVector({State::Committed, State::Outdated}, &state);

        return data->getAllDataPartsVector(&state);
    }

    return data->getDataPartsVector({State::Committed}, &state);
}

StoragesInfoStream::StoragesInfoStream(const SelectQueryInfo & query_info, const Context & context)
    : query_id(context.getCurrentQueryId())
{
    /// Will apply WHERE to subset of columns and then add more columns.
    /// This is kind of complicated, but we use WHERE to do less work.

    Block block_to_filter;

    MutableColumnPtr table_column_mut = ColumnString::create();
    MutableColumnPtr engine_column_mut = ColumnString::create();
    MutableColumnPtr active_column_mut = ColumnUInt8::create();

    {
        Databases databases = context.getDatabases();

        /// Add column 'database'.
        MutableColumnPtr database_column_mut = ColumnString::create();
        for (const auto & database : databases)
        {
            if (context.hasDatabaseAccessRights(database.first))
                database_column_mut->insert(database.first);
        }
        block_to_filter.insert(ColumnWithTypeAndName(
            std::move(database_column_mut), std::make_shared<DataTypeString>(), "database"));

        /// Filter block_to_filter with column 'database'.
        VirtualColumnUtils::filterBlockWithQuery(query_info.query, block_to_filter, context);
        rows = block_to_filter.rows();

        /// Block contains new columns, update database_column.
        ColumnPtr database_column_ = block_to_filter.getByName("database").column;

        if (rows)
        {
            /// Add columns 'table', 'engine', 'active'

            IColumn::Offsets offsets(rows);

            for (size_t i = 0; i < rows; ++i)
            {
                String database_name = (*database_column_)[i].get<String>();
                const DatabasePtr database = databases.at(database_name);

                offsets[i] = i ? offsets[i - 1] : 0;
                for (auto iterator = database->getIterator(context); iterator->isValid(); iterator->next())
                {
                    String table_name = iterator->name();
                    StoragePtr storage = iterator->table();
                    String engine_name = storage->getName();

                    if (!dynamic_cast<MergeTreeData *>(storage.get()))
                        continue;

                    storages[std::make_pair(database_name, iterator->name())] = storage;

                    /// Add all combinations of flag 'active'.
                    for (UInt64 active : {0, 1})
                    {
                        table_column_mut->insert(table_name);
                        engine_column_mut->insert(engine_name);
                        active_column_mut->insert(active);
                    }

                    offsets[i] += 2;
                }
            }

            for (size_t i = 0; i < block_to_filter.columns(); ++i)
            {
                ColumnPtr & column = block_to_filter.safeGetByPosition(i).column;
                column = column->replicate(offsets);
            }
        }
    }

    block_to_filter.insert(ColumnWithTypeAndName(std::move(table_column_mut), std::make_shared<DataTypeString>(), "table"));
    block_to_filter.insert(ColumnWithTypeAndName(std::move(engine_column_mut), std::make_shared<DataTypeString>(), "engine"));
    block_to_filter.insert(ColumnWithTypeAndName(std::move(active_column_mut), std::make_shared<DataTypeUInt8>(), "active"));

    if (rows)
    {
        /// Filter block_to_filter with columns 'database', 'table', 'engine', 'active'.
        VirtualColumnUtils::filterBlockWithQuery(query_info.query, block_to_filter, context);
        rows = block_to_filter.rows();
    }

    database_column = block_to_filter.getByName("database").column;
    table_column = block_to_filter.getByName("table").column;
    active_column = block_to_filter.getByName("active").column;

    next_row = 0;
}

StoragesInfo StoragesInfoStream::next()
{
    while (next_row < rows)
    {
        StoragesInfo info;

        info.database = (*database_column)[next_row].get<String>();
        info.table = (*table_column)[next_row].get<String>();

        auto isSameTable = [&info, this] (size_t row) -> bool
        {
            return (*database_column)[row].get<String>() == info.database &&
                   (*table_column)[row].get<String>() == info.table;
        };

        /// We may have two rows per table which differ in 'active' value.
        /// If rows with 'active = 0' were not filtered out, this means we
        /// must collect the inactive parts. Remember this fact in StoragesInfo.
        for (; next_row < rows && isSameTable(next_row); ++next_row)
        {
            const auto active = (*active_column)[next_row].get<UInt64>();
            if (active == 0)
                info.need_inactive_parts = true;
        }

        info.storage = storages.at(std::make_pair(info.database, info.table));

        try
        {
            /// For table not to be dropped and set of columns to remain constant.
            info.table_lock = info.storage->lockStructureForShare(false, query_id);
        }
        catch (const Exception & e)
        {
            /** There are case when IStorage::drop was called,
              *  but we still own the object.
              * Then table will throw exception at attempt to lock it.
              * Just skip the table.
              */
            if (e.code() == ErrorCodes::TABLE_IS_DROPPED)
                continue;

            throw;
        }

        info.engine = info.storage->getName();

        info.data = dynamic_cast<MergeTreeData *>(info.storage.get());
        if (!info.data)
            throw Exception("Unknown engine " + info.engine, ErrorCodes::LOGICAL_ERROR);

        return info;
    }

    return {};
}

BlockInputStreams StorageSystemPartsBase::read(
        const Names & column_names,
        const SelectQueryInfo & query_info,
        const Context & context,
        QueryProcessingStage::Enum /*processed_stage*/,
        const size_t /*max_block_size*/,
        const unsigned /*num_streams*/)
{
    bool has_state_column = hasStateColumn(column_names);

    StoragesInfoStream stream(query_info, context);

    /// Create the result.

    MutableColumns res_columns = getSampleBlock().cloneEmptyColumns();
    if (has_state_column)
        res_columns.push_back(ColumnString::create());

    while (StoragesInfo info = stream.next())
    {
        processNextStorage(res_columns, info, has_state_column);
    }

    Block block = getSampleBlock();
    if (has_state_column)
        block.insert(ColumnWithTypeAndName(std::make_shared<DataTypeString>(), "_state"));

    return BlockInputStreams(1, std::make_shared<OneBlockInputStream>(block.cloneWithColumns(std::move(res_columns))));
}

NameAndTypePair StorageSystemPartsBase::getColumn(const String & column_name) const
{
    if (column_name == "_state")
        return NameAndTypePair("_state", std::make_shared<DataTypeString>());

    return IStorage::getColumn(column_name);
}

bool StorageSystemPartsBase::hasColumn(const String & column_name) const
{
    if (column_name == "_state")
        return true;

    return IStorage::hasColumn(column_name);
}

StorageSystemPartsBase::StorageSystemPartsBase(std::string name_, NamesAndTypesList && columns_)
    : name(std::move(name_))
{
    ColumnsDescription tmp_columns(std::move(columns_));

    auto add_alias = [&](const String & alias_name, const String & column_name)
    {
        ColumnDescription column(alias_name, tmp_columns.get(column_name).type, false);
        column.default_desc.kind = ColumnDefaultKind::Alias;
        column.default_desc.expression = std::make_shared<ASTIdentifier>(column_name);
        tmp_columns.add(column);
    };

    /// Add aliases for old column names for backwards compatibility.
    add_alias("bytes", "bytes_on_disk");
    add_alias("marks_size", "marks_bytes");

    setColumns(tmp_columns);
}

}
