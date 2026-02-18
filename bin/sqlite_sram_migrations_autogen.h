#ifndef SQLITE_SRAM_MIGRATIONS_AUTOGEN_H
#define SQLITE_SRAM_MIGRATIONS_AUTOGEN_H

#include <stddef.h>

#include "support/sqlite_sram/migrations.h"

const sqlite_sram_migration_t g_sqlite_sram_migrations[] = {
	{"202602180001_create_snapshots.sql", R"__MIG__(
CREATE TABLE IF NOT EXISTS snapshots (
	id INTEGER PRIMARY KEY,
	ts_ms INTEGER NOT NULL,
	crc32 INTEGER NOT NULL,
	sram BLOB NOT NULL
);
)__MIG__"},
	{"202602180002_add_snapshots_tag.sql", R"__MIG__(
ALTER TABLE snapshots ADD COLUMN tag TEXT DEFAULT NULL;
)__MIG__"},
};

const size_t g_sqlite_sram_migrations_count = sizeof(g_sqlite_sram_migrations) / sizeof(g_sqlite_sram_migrations[0]);

#endif
