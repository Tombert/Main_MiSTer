#ifndef SQLITE_SRAM_MIGRATIONS_H
#define SQLITE_SRAM_MIGRATIONS_H

#include <stddef.h>

struct sqlite_sram_migration_t
{
	const char *name;
	const char *sql;
};

extern const sqlite_sram_migration_t g_sqlite_sram_migrations[];
extern const size_t g_sqlite_sram_migrations_count;

#endif
