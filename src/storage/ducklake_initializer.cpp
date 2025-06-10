#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include "storage/ducklake_initializer.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_schema_entry.hpp"

namespace duckdb {

DuckLakeInitializer::DuckLakeInitializer(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeOptions &options_p)
    : context(context), catalog(catalog), options(options_p) {
	InitializeDataPath();
}

string DuckLakeInitializer::GetAttachOptions() {
	vector<string> attach_options;
	if (options.access_mode != AccessMode::AUTOMATIC) {
		switch (options.access_mode) {
		case AccessMode::READ_ONLY:
			attach_options.push_back("READ_ONLY");
			break;
		case AccessMode::READ_WRITE:
			attach_options.push_back("READ_WRITE");
			break;
		default:
			throw InternalException("Unsupported access mode in DuckLake attach");
		}
	}
	for (auto &option : options.metadata_parameters) {
		attach_options.push_back(option.first + " " + option.second.ToSQLString());
	}

	if (attach_options.empty()) {
		return string();
	}
	string result;
	for (auto &option : attach_options) {
		if (!result.empty()) {
			result += ", ";
		}
		result += option;
	}
	return " (" + result + ")";
}

void DuckLakeInitializer::Initialize() {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	// explicitly load all secrets - work-around to secret initialization bug
	transaction.Query("FROM duckdb_secrets()");
	// attach the metadata database
	auto result =
	    transaction.Query("ATTACH {METADATA_PATH} AS {METADATA_CATALOG_NAME_IDENTIFIER}" + GetAttachOptions());
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to attach DuckLake MetaData \"" + catalog.MetadataDatabaseName() + "\" at path + \"" +
		                catalog.MetadataPath() + "\"");
	}
	bool has_explicit_schema = !options.metadata_schema.empty();
	if (options.metadata_schema.empty()) {
		// if the schema is not explicitly set by the user - set it to the default schema in the catalog
		options.metadata_schema = transaction.GetDefaultSchemaName();
	}
	if (options.data_inlining_row_limit > 0) {
		auto &metadata_catalog = Catalog::GetCatalog(*transaction.GetConnection().context, options.metadata_database);
		if (!metadata_catalog.IsDuckCatalog()) {
			throw NotImplementedException("Data inlining is currently only supported on DuckDB catalogs");
		}
	}
	// after the metadata database is attached initialize the ducklake
	// check if we are loading an existing DuckLake or creating a new one
	// FIXME: verify that all tables are in the correct format instead
	result = transaction.Query(
	    "SELECT COUNT(*) FROM duckdb_tables() WHERE database_name={METADATA_CATALOG_NAME_LITERAL} AND "
	    "schema_name={METADATA_SCHEMA_NAME_LITERAL} AND table_name LIKE 'ducklake_%'");
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to load DuckLake table data");
	}
	auto count = result->Fetch()->GetValue(0, 0).GetValue<idx_t>();
	if (count == 0) {
		InitializeNewDuckLake(transaction, has_explicit_schema);
	} else {
		LoadExistingDuckLake(transaction);
	}
	if (options.at_clause) {
		// if the user specified a snapshot try to load it to trigger an error if it does not exist
		transaction.GetSnapshot();
	}
}

void DuckLakeInitializer::InitializeDataPath() {
	auto &data_path = options.data_path;
	if (data_path.empty()) {
		return;
	}

	// This functions will:
	//	1. Check if a known extension pattern matches the start of the data_path
	//	2. If so, either load the required extension or throw a relevant error message
	CheckAndAutoloadedRequiredExtension(data_path);

	auto &fs = FileSystem::GetFileSystem(context);
	auto separator = fs.PathSeparator(data_path);
	// ensure the paths we store always end in a path separator
	if (!StringUtil::EndsWith(data_path, separator)) {
		data_path += separator;
	}
	catalog.Separator() = separator;
}

void DuckLakeInitializer::InitializeNewDuckLake(DuckLakeTransaction &transaction, bool has_explicit_schema) {
	if (options.data_path.empty()) {
		auto &metadata_catalog = Catalog::GetCatalog(*transaction.GetConnection().context, options.metadata_database);
		if (!metadata_catalog.IsDuckCatalog()) {
			throw InvalidInputException(
			    "Attempting to create a new ducklake instance but data_path is not set - set the "
			    "DATA_PATH parameter to the desired location of the data files");
		}
		// for DuckDB instances - use a default data path
		auto path = metadata_catalog.GetAttached().GetStorageManager().GetDBPath();
		options.data_path = path + ".files";
		InitializeDataPath();
	}
	auto &metadata_manager = transaction.GetMetadataManager();
	metadata_manager.InitializeDuckLake(has_explicit_schema, catalog.Encryption());
	if (catalog.Encryption() == DuckLakeEncryption::AUTOMATIC) {
		// default to unencrypted
		catalog.SetEncryption(DuckLakeEncryption::UNENCRYPTED);
	}
}

void DuckLakeInitializer::LoadExistingDuckLake(DuckLakeTransaction &transaction) {
	// load the data path from the existing duck lake
	auto &metadata_manager = transaction.GetMetadataManager();
	auto metadata = metadata_manager.LoadDuckLake();
	for (auto &tag : metadata.tags) {
		if (tag.key == "version") {
			string version = tag.value;
			if (version == "0.1") {
				metadata_manager.MigrateV01();
				version = "0.2";
			}
			if (version != "0.2") {
				throw NotImplementedException("Only DuckLake versions 0.1 and 0.2 are supported");
			}
		}
		if (tag.key == "data_path") {
			if (options.data_path.empty()) {
				options.data_path = metadata_manager.LoadPath(tag.value);
				InitializeDataPath();
			}
		}
		if (tag.key == "encrypted") {
			if (tag.value == "true") {
				catalog.SetEncryption(DuckLakeEncryption::ENCRYPTED);
			} else if (tag.value == "false") {
				catalog.SetEncryption(DuckLakeEncryption::UNENCRYPTED);
			} else {
				throw NotImplementedException("Encrypted should be either true or false");
			}
		}
		options.config_options.emplace(tag.key, tag.value);
	}
	for (auto &entry : metadata.schema_settings) {
		options.schema_options[entry.schema_id].emplace(entry.tag.key, entry.tag.value);
	}
	for (auto &entry : metadata.table_settings) {
		options.table_options[entry.table_id].emplace(entry.tag.key, entry.tag.value);
	}
}

} // namespace duckdb
