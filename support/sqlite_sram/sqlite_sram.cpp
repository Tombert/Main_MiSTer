#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <vector>

#include "support/sqlite_sram/sqlite_sram.h"
#include "support/sqlite_sram/migrations.h"

#include "cfg.h"
#include "hardware.h"
#include "file_io.h"
#include "user_io.h"
#include "miniz.h"

#if SQLITE_SRAM_SNAPSHOTS
#include <sqlite3.h>

#define SQLITE_SRAM_MAX_SLOTS      16
#define SQLITE_SRAM_HISTORY_LIMIT  50
#define SQLITE_SRAM_RETRY_MS       60000

struct sqlite_sram_slot_t
{
	bool enabled = false;
	bool dirty = false;
	uint32_t flush_timer = 0;
	fileTYPE *img = nullptr;
	char save_path[2048] = {};
	char db_path[2080] = {};
};

struct sqlite_sram_autosave_t
{
	bool scanned = false;
	bool found = false;
	bool ex = false;
	uint32_t timer = 0;
	char opt[32] = {};
	char label[96] = {};
};

static sqlite_sram_slot_t g_slots[SQLITE_SRAM_MAX_SLOTS] = {};
static sqlite_sram_autosave_t g_autosave = {};

static uint32_t sqlite_sram_interval_ms()
{
	uint32_t interval_sec = cfg.sqlite_sram_autosave_interval;
	if (!interval_sec) interval_sec = 300;
	if (interval_sec > (UINT32_MAX / 1000)) interval_sec = UINT32_MAX / 1000;
	return interval_sec * 1000;
}

static int64_t sqlite_sram_timestamp_ms()
{
	struct timespec ts = {};
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t sqlite_sram_crc32(const uint8_t *data, size_t size)
{
	if (!data || !size) return 0;
	return (uint32_t)mz_crc32(MZ_CRC32_INIT, data, size);
}

static bool sqlite_sram_exec(sqlite3 *db, const char *sql)
{
	char *err = nullptr;
	int rc = sqlite3_exec(db, sql, 0, 0, &err);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQLite SRAM sqlite error: %s [%s]\n", err ? err : "(unknown)", sql);
		if (err) sqlite3_free(err);
		return false;
	}
	return true;
}

static bool sqlite_sram_migration_applied(sqlite3 *db, const char *name, bool *applied)
{
	if (!db || !name || !applied) return false;
	*applied = false;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM schema_migrations WHERE name = ?1 LIMIT 1;", -1, &stmt, 0) != SQLITE_OK)
	{
		return false;
	}

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	const int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) *applied = true;
	else if (rc != SQLITE_DONE)
	{
		sqlite3_finalize(stmt);
		return false;
	}

	sqlite3_finalize(stmt);
	return true;
}

static bool sqlite_sram_record_migration(sqlite3 *db, const char *name)
{
	if (!db || !name) return false;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, "INSERT INTO schema_migrations(name, applied_ts_ms) VALUES(?1, ?2);", -1, &stmt, 0) != SQLITE_OK)
	{
		return false;
	}

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, sqlite_sram_timestamp_ms());
	const int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE;
}

static bool sqlite_sram_apply_migrations(sqlite3 *db)
{
	if (!db) return false;

	if (!sqlite_sram_exec(db, "CREATE TABLE IF NOT EXISTS schema_migrations (name TEXT PRIMARY KEY, applied_ts_ms INTEGER NOT NULL);")) return false;

	std::vector<const sqlite_sram_migration_t*> ordered;
	ordered.reserve(g_sqlite_sram_migrations_count);
	for (size_t i = 0; i < g_sqlite_sram_migrations_count; i++)
	{
		ordered.push_back(&g_sqlite_sram_migrations[i]);
	}

	std::sort(ordered.begin(), ordered.end(), [](const sqlite_sram_migration_t *a, const sqlite_sram_migration_t *b)
	{
		return strcmp(a->name, b->name) < 0;
	});

	for (size_t i = 1; i < ordered.size(); i++)
	{
		if (!strcmp(ordered[i - 1]->name, ordered[i]->name))
		{
			fprintf(stderr, "SQLite SRAM migration error: duplicate migration name %s\n", ordered[i]->name);
			return false;
		}
	}

	for (const sqlite_sram_migration_t *migration : ordered)
	{
		bool applied = false;
		if (!sqlite_sram_migration_applied(db, migration->name, &applied))
		{
			fprintf(stderr, "SQLite SRAM migration error: failed to query %s\n", migration->name);
			return false;
		}
		if (applied) continue;

		if (!sqlite_sram_exec(db, "BEGIN IMMEDIATE;")) return false;
		bool ok = sqlite_sram_exec(db, migration->sql);
		if (ok) ok = sqlite_sram_record_migration(db, migration->name);
		if (ok) ok = sqlite_sram_exec(db, "COMMIT;");
		if (!ok)
		{
			sqlite_sram_exec(db, "ROLLBACK;");
			fprintf(stderr, "SQLite SRAM migration error: failed applying %s\n", migration->name);
			return false;
		}

		fprintf(stderr, "SQLite SRAM migration applied: %s\n", migration->name);
	}

	return true;
}

static bool sqlite_sram_prepare_db(sqlite3 *db)
{
	if (!sqlite_sram_exec(db, "PRAGMA journal_mode=PERSIST;")) return false;
	if (!sqlite_sram_exec(db, "PRAGMA synchronous=FULL;")) return false;
	if (!sqlite_sram_exec(db, "PRAGMA auto_vacuum=NONE;")) return false;
	if (!sqlite_sram_exec(db, "PRAGMA temp_store=MEMORY;")) return false;
	if (!sqlite_sram_exec(db, "PRAGMA journal_size_limit=1048576;")) return false;
	return sqlite_sram_apply_migrations(db);
}

static bool sqlite_sram_open_db(const char *db_path, sqlite3 **db)
{
	if (!db_path || !db_path[0] || !db) return false;

	char full_db_path[2100] = {};
	snprintf(full_db_path, sizeof(full_db_path), "%s", getFullPath(db_path));

	sqlite3 *tmp_db = nullptr;
	int rc = sqlite3_open_v2(full_db_path, &tmp_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQLite SRAM sqlite error: cannot open %s\n", full_db_path);
		if (tmp_db) sqlite3_close(tmp_db);
		return false;
	}

	sqlite3_busy_timeout(tmp_db, 10000);
	if (!sqlite_sram_prepare_db(tmp_db))
	{
		sqlite3_close(tmp_db);
		return false;
	}

	*db = tmp_db;
	return true;
}

static bool sqlite_sram_latest_matches(sqlite3 *db, const std::vector<uint8_t> &data, bool *matches)
{
	if (!db || !matches) return false;
	*matches = false;

	const uint32_t data_crc = sqlite_sram_crc32(data.data(), data.size());

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, "SELECT crc32, sram FROM snapshots ORDER BY id DESC LIMIT 1;", -1, &stmt, 0) != SQLITE_OK)
	{
		return false;
	}

	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
	{
		const uint32_t stored_crc = (uint32_t)sqlite3_column_int64(stmt, 0);
		const int blob_size = sqlite3_column_bytes(stmt, 1);
		const void *blob = sqlite3_column_blob(stmt, 1);
		if (stored_crc == data_crc && blob_size == (int)data.size() && (!blob_size || (blob && !memcmp(blob, data.data(), blob_size))))
		{
			*matches = true;
		}
	}
	else if (rc != SQLITE_DONE)
	{
		sqlite3_finalize(stmt);
		return false;
	}

	sqlite3_finalize(stmt);
	return true;
}

static bool sqlite_sram_insert(sqlite3 *db, const std::vector<uint8_t> &data)
{
	if (!db) return false;
	if (!sqlite_sram_exec(db, "BEGIN IMMEDIATE;")) return false;

	const uint32_t data_crc = sqlite_sram_crc32(data.data(), data.size());

	bool ok = true;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, "INSERT INTO snapshots(ts_ms, crc32, sram) VALUES(?, ?, ?);", -1, &stmt, 0) != SQLITE_OK)
	{
		ok = false;
	}
	else
	{
		sqlite3_bind_int64(stmt, 1, sqlite_sram_timestamp_ms());
		sqlite3_bind_int64(stmt, 2, data_crc);
		sqlite3_bind_blob(stmt, 3, data.data(), (int)data.size(), SQLITE_TRANSIENT);
		const int rc = sqlite3_step(stmt);
		ok = (rc == SQLITE_DONE);
		sqlite3_finalize(stmt);
	}

	if (ok)
	{
		char sql[320] = {};
		snprintf(sql, sizeof(sql),
			"DELETE FROM snapshots "
			"WHERE tag IS NULL "
			"AND id NOT IN (SELECT id FROM snapshots WHERE tag IS NULL ORDER BY id DESC LIMIT %d);",
			SQLITE_SRAM_HISTORY_LIMIT);
		if (!sqlite_sram_exec(db, sql)) ok = false;
	}
	if (ok && !sqlite_sram_exec(db, "COMMIT;")) ok = false;

	if (!ok)
	{
		sqlite_sram_exec(db, "ROLLBACK;");
		return false;
	}

	return true;
}

static bool sqlite_sram_load_latest(const char *db_path, std::vector<uint8_t> &data, bool *found)
{
	if (!db_path || !found) return false;
	*found = false;
	data.clear();

	char full_db_path[2100] = {};
	snprintf(full_db_path, sizeof(full_db_path), "%s", getFullPath(db_path));

	struct stat st = {};
	if (stat(full_db_path, &st) < 0)
	{
		return true;
	}

	sqlite3 *db = nullptr;
	int rc = sqlite3_open_v2(full_db_path, &db, SQLITE_OPEN_READONLY, 0);
	if (rc != SQLITE_OK)
	{
		if (db) sqlite3_close(db);
		db = nullptr;
		rc = sqlite3_open_v2(full_db_path, &db, SQLITE_OPEN_READWRITE, 0);
	}
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQLite SRAM sqlite error: cannot open for load %s\n", full_db_path);
		if (db) sqlite3_close(db);
		return false;
	}

	sqlite3_stmt *stmt = nullptr;
	bool ok = true;
	if (sqlite3_prepare_v2(db, "SELECT id, sram, crc32 FROM snapshots ORDER BY id DESC;", -1, &stmt, 0) != SQLITE_OK)
	{
		ok = false;
	}
	else
	{
		while (1)
		{
			const int step_rc = sqlite3_step(stmt);
			if (step_rc == SQLITE_DONE) break;
			if (step_rc != SQLITE_ROW)
			{
				ok = false;
				break;
			}

			const int64_t row_id = sqlite3_column_int64(stmt, 0);
			const int blob_size = sqlite3_column_bytes(stmt, 1);
			const uint8_t *blob = (const uint8_t*)sqlite3_column_blob(stmt, 1);
			const uint32_t stored_crc = (uint32_t)sqlite3_column_int64(stmt, 2);

			if (blob_size > 0 && !blob)
			{
				fprintf(stderr, "SQLite SRAM load skip: %s row=%lld (null blob)\n", db_path, (long long)row_id);
				continue;
			}

			const uint32_t calc_crc = sqlite_sram_crc32(blob, blob_size > 0 ? (size_t)blob_size : 0);
			if (calc_crc != stored_crc)
			{
				fprintf(stderr, "SQLite SRAM load skip: %s row=%lld crc mismatch stored=%08X calc=%08X\n",
					db_path, (long long)row_id, stored_crc, calc_crc);
				continue;
			}

			if (blob_size > 0) data.assign(blob, blob + blob_size);
			else data.clear();

			*found = true;
			fprintf(stderr, "SQLite SRAM load row: %s row=%lld (%d bytes, crc=%08X)\n",
				db_path, (long long)row_id, blob_size, stored_crc);
			break;
		}

		if (ok && !*found)
		{
			fprintf(stderr, "SQLite SRAM load: no valid rows in %s\n", db_path);
		}
		sqlite3_finalize(stmt);
	}

	sqlite3_close(db);
	return ok;
}

static bool sqlite_sram_read_image(fileTYPE *img, std::vector<uint8_t> &data)
{
	if (!img || !img->filp) return false;
	if (img->size < 0 || img->size > INT_MAX) return false;

	const __off64_t old_offset = img->offset;
	const int image_size = (int)img->size;
	data.resize(image_size);

	bool ok = false;
	if (FileSeek(img, 0, SEEK_SET))
	{
		if (image_size > 0)
		{
			const int read_size = FileReadAdv(img, data.data(), image_size, -1);
			ok = (read_size == image_size);
		}
		else
		{
			ok = true;
		}
	}

	FileSeek(img, old_offset, SEEK_SET);
	if (!ok) data.clear();
	return ok;
}

static bool sqlite_sram_write_image(fileTYPE *img, const uint8_t *data, size_t size)
{
	if (!img || !img->filp) return false;
	if (!FileSeek(img, 0, SEEK_SET)) return false;

	if (size)
	{
		const int write_size = FileWriteAdv(img, (void*)data, (int)size, -1);
		if (write_size != (int)size) return false;
	}

	return FileSeek(img, 0, SEEK_SET);
}

static bool sqlite_sram_fill_ff(fileTYPE *img, int total_bytes)
{
	if (!img || !img->filp || total_bytes <= 0) return false;
	if (!FileSeek(img, 0, SEEK_SET)) return false;

	static uint8_t ff_buf[4096] = {};
	static bool ff_init = false;
	if (!ff_init)
	{
		memset(ff_buf, 0xFF, sizeof(ff_buf));
		ff_init = true;
	}

	int remaining = total_bytes;
	while (remaining > 0)
	{
		int chunk = (remaining > (int)sizeof(ff_buf)) ? (int)sizeof(ff_buf) : remaining;
		if (FileWriteAdv(img, ff_buf, chunk, -1) != chunk) return false;
		remaining -= chunk;
	}

	return FileSeek(img, 0, SEEK_SET);
}

static bool sqlite_sram_migrate_legacy_save(const char *save_path, const char *db_path)
{
	if (!save_path || !save_path[0] || !db_path || !db_path[0]) return true;
	if (FileExists(db_path, 0)) return true;
	if (!FileExists(save_path, 0)) return true;

	fileTYPE legacy_file = {};
	if (!FileOpenEx(&legacy_file, save_path, O_RDONLY))
	{
		fprintf(stderr, "SQLite SRAM migration warning: failed to open legacy save %s\n", save_path);
		return false;
	}

	if (legacy_file.size < 0 || legacy_file.size > INT_MAX)
	{
		fprintf(stderr, "SQLite SRAM migration warning: invalid legacy save size %lld for %s\n",
			(long long)legacy_file.size, save_path);
		FileClose(&legacy_file);
		return false;
	}

	const int legacy_size = (int)legacy_file.size;
	std::vector<uint8_t> legacy_data((size_t)legacy_size);
	bool read_ok = true;
	if (legacy_size > 0)
	{
		const int read_size = FileReadAdv(&legacy_file, legacy_data.data(), legacy_size, -1);
		read_ok = (read_size == legacy_size);
	}
	FileClose(&legacy_file);

	if (!read_ok)
	{
		fprintf(stderr, "SQLite SRAM migration warning: failed to read legacy save %s\n", save_path);
		return false;
	}

	sqlite3 *db = nullptr;
	if (!sqlite_sram_open_db(db_path, &db))
	{
		fprintf(stderr, "SQLite SRAM migration warning: failed to open sqlite DB %s\n", db_path);
		return false;
	}

	bool ok = sqlite_sram_insert(db, legacy_data);
	sqlite3_close(db);

	if (!ok)
	{
		char full_db_path[2100] = {};
		snprintf(full_db_path, sizeof(full_db_path), "%s", getFullPath(db_path));
		remove(full_db_path);
		fprintf(stderr, "SQLite SRAM migration warning: failed to import %s into %s\n", save_path, db_path);
		return false;
	}

	fprintf(stderr, "SQLite SRAM migrated legacy save: %s -> %s (%d bytes)\n", save_path, db_path, legacy_size);
	return true;
}

static bool sqlite_sram_run_db_migrations(const char *db_path)
{
	if (!db_path || !db_path[0]) return false;
	if (!FileExists(db_path, 0)) return true;

	sqlite3 *db = nullptr;
	if (!sqlite_sram_open_db(db_path, &db))
	{
		fprintf(stderr, "SQLite SRAM migration warning: failed to apply migrations to %s\n", db_path);
		return false;
	}

	sqlite3_close(db);
	return true;
}

static void sqlite_sram_configure_slot(uint8_t slot, fileTYPE *img, const char *save_path)
{
	if (slot >= SQLITE_SRAM_MAX_SLOTS) return;

	sqlite_sram_slot_t &state = g_slots[slot];
	memset(&state, 0, sizeof(state));
	state.img = img;

	if (!save_path || !save_path[0]) return;

	state.enabled = true;
	snprintf(state.save_path, sizeof(state.save_path), "%s", save_path);
	snprintf(state.db_path, sizeof(state.db_path), "%s.sqlite3", save_path);
}

static bool sqlite_sram_any_slot_enabled()
{
	for (uint8_t i = 0; i < SQLITE_SRAM_MAX_SLOTS; i++)
	{
		if (g_slots[i].enabled) return true;
	}
	return false;
}

static bool sqlite_sram_ci_contains(const char *text, const char *needle)
{
	if (!text || !needle || !needle[0]) return false;

	for (size_t i = 0; text[i]; i++)
	{
		size_t j = 0;
		while (needle[j] && text[i + j] && (tolower((unsigned char)text[i + j]) == tolower((unsigned char)needle[j]))) j++;
		if (!needle[j]) return true;
	}

	return false;
}

static int sqlite_sram_export_trigger_score(const char *label)
{
	if (!label || !label[0]) return -1000;

	const bool has_load = sqlite_sram_ci_contains(label, "load") || sqlite_sram_ci_contains(label, "restore");
	if (has_load) return -1000;

	const bool has_save = sqlite_sram_ci_contains(label, "save");
	const bool has_write = sqlite_sram_ci_contains(label, "write");
	const bool has_sram = sqlite_sram_ci_contains(label, "sram");
	const bool has_nvram = sqlite_sram_ci_contains(label, "nvram");
	const bool has_backup_ram = sqlite_sram_ci_contains(label, "backup ram");
	const bool has_memory_card = sqlite_sram_ci_contains(label, "memory card") || sqlite_sram_ci_contains(label, "memcard");
	const bool has_save_ram = sqlite_sram_ci_contains(label, "save ram");
	const bool has_storage = has_sram || has_nvram || has_backup_ram || has_memory_card || has_save_ram;

	if (!(has_save || has_write)) return -1000;
	if (!(has_storage || has_sram || has_nvram)) return -1000;

	int score = 0;
	if (has_save) score += 100;
	if (has_write) score += 80;
	if (has_sram) score += 30;
	if (has_nvram) score += 30;
	if (has_backup_ram) score += 30;
	if (has_memory_card) score += 20;
	if (has_save_ram) score += 20;

	if (sqlite_sram_ci_contains(label, "state")) score -= 80;
	if (sqlite_sram_ci_contains(label, "setting")) score -= 80;
	if (sqlite_sram_ci_contains(label, "config")) score -= 80;

	return score;
}

static bool sqlite_sram_find_export_trigger(char *opt_out, size_t opt_size, bool *ex_out, char *label_out, size_t label_size)
{
	int best_score = -1000;
	char best_opt[32] = {};
	char best_label[96] = {};
	bool best_ex = false;

	for (int i = 2;; i++)
	{
		char *entry = user_io_get_confstr(i);
		if (!entry) break;

		while ((entry[0] == 'H' || entry[0] == 'D' || entry[0] == 'h' || entry[0] == 'd') && strlen(entry) > 2) entry += 2;
		if (entry[0] == 'P' && strlen(entry) > 2 && entry[2] != ',') entry += 2;

		if (!(entry[0] == 'T' || entry[0] == 't' || entry[0] == 'R' || entry[0] == 'r')) continue;

		char label[256] = {};
		substrcpy(label, entry, 1);
		int score = sqlite_sram_export_trigger_score(label);
		if (score <= -1000) continue;

		bool ex = (entry[0] == 't') || (entry[0] == 'r');
		char opt[32] = {};
		substrcpy(opt, entry + 1, 0);
		if (!opt[0]) continue;

		fprintf(stderr, "SQLite SRAM autosave candidate: opt=%s ex=%d label=%s score=%d\n", opt, ex ? 1 : 0, label, score);

		if (score <= best_score) continue;
		best_score = score;
		best_ex = ex;
		snprintf(best_opt, sizeof(best_opt), "%s", opt);
		snprintf(best_label, sizeof(best_label), "%s", label);
	}

	if (best_score <= -1000) return false;

	snprintf(opt_out, opt_size, "%s", best_opt);
	*ex_out = best_ex;
	snprintf(label_out, label_size, "%s", best_label);
	return true;
}

static void sqlite_sram_poll_export_trigger()
{
	if (!sqlite_sram_any_slot_enabled())
	{
		g_autosave.timer = 0;
		return;
	}

	if (!g_autosave.scanned)
	{
		g_autosave.scanned = true;
		user_io_read_confstr();
		g_autosave.found = sqlite_sram_find_export_trigger(g_autosave.opt, sizeof(g_autosave.opt), &g_autosave.ex, g_autosave.label, sizeof(g_autosave.label));
		if (g_autosave.found)
		{
			fprintf(stderr, "SQLite SRAM autosave trigger found: opt=%s ex=%d label=%s\n", g_autosave.opt, g_autosave.ex ? 1 : 0, g_autosave.label);
		}
		else
		{
			fprintf(stderr, "SQLite SRAM autosave trigger not found in core config string.\n");
		}
	}

	if (!g_autosave.found) return;

	if (!g_autosave.timer)
	{
		g_autosave.timer = GetTimer(sqlite_sram_interval_ms());
		return;
	}

	if (!CheckTimer(g_autosave.timer)) return;

	g_autosave.timer = GetTimer(sqlite_sram_interval_ms());
	user_io_status_set(g_autosave.opt, 1, g_autosave.ex);
	user_io_status_set(g_autosave.opt, 0, g_autosave.ex);
	fprintf(stderr, "SQLite SRAM autosave trigger fired: opt=%s label=%s\n", g_autosave.opt, g_autosave.label);
}

static void sqlite_sram_try_flush(uint8_t slot)
{
	if (slot >= SQLITE_SRAM_MAX_SLOTS) return;

	sqlite_sram_slot_t &state = g_slots[slot];
	if (!state.enabled || !state.dirty || !state.img) return;

	std::vector<uint8_t> data;
	if (!sqlite_sram_read_image(state.img, data))
	{
		state.flush_timer = GetTimer(SQLITE_SRAM_RETRY_MS);
		return;
	}

	sqlite3 *db = nullptr;
	if (!sqlite_sram_open_db(state.db_path, &db))
	{
		state.flush_timer = GetTimer(SQLITE_SRAM_RETRY_MS);
		return;
	}

	bool ok = true;
	bool unchanged = false;
	if (ok) ok = sqlite_sram_latest_matches(db, data, &unchanged);
	if (ok && !unchanged) ok = sqlite_sram_insert(db, data);
	sqlite3_close(db);

	if (!ok)
	{
		state.flush_timer = GetTimer(SQLITE_SRAM_RETRY_MS);
		return;
	}

	state.dirty = false;
	state.flush_timer = 0;

	if (!unchanged) fprintf(stderr, "SQLite SRAM saved: %s (%d bytes)\n", state.save_path, (int)data.size());
	else fprintf(stderr, "SQLite SRAM unchanged: %s (%d bytes)\n", state.save_path, (int)data.size());
}

static void sqlite_sram_poll_flush()
{
	for (uint8_t i = 0; i < SQLITE_SRAM_MAX_SLOTS; i++)
	{
		sqlite_sram_slot_t &state = g_slots[i];
		if (!state.enabled || !state.dirty || !state.flush_timer) continue;
		if (!CheckTimer(state.flush_timer)) continue;
		sqlite_sram_try_flush(i);
	}
}

#endif

int sqlite_sram_runtime_enabled()
{
#if SQLITE_SRAM_SNAPSHOTS
	return cfg.sqlite_sram_enable ? 1 : 0;
#else
	return 0;
#endif
}

void sqlite_sram_reset()
{
#if SQLITE_SRAM_SNAPSHOTS
	memset(g_slots, 0, sizeof(g_slots));
	memset(&g_autosave, 0, sizeof(g_autosave));
#endif
}

bool sqlite_sram_mount_virtual(uint8_t slot, const char *save_path, int pre_size, fileTYPE *img)
{
#if SQLITE_SRAM_SNAPSHOTS
	if (!sqlite_sram_runtime_enabled()) return false;
	if (slot >= SQLITE_SRAM_MAX_SLOTS || !save_path || !save_path[0] || !img) return false;

	sqlite_sram_configure_slot(slot, img, save_path);

	char tmp_path[64] = {};
	snprintf(tmp_path, sizeof(tmp_path), "/tmp/mister_sram_slot_%u.bin", slot);

	if (!FileOpenEx(img, tmp_path, O_CREAT | O_RDWR | O_TRUNC | O_SYNC, 1, 0))
	{
		fprintf(stderr, "SQLite SRAM error: failed to create temporary save image \"%s\".\n", tmp_path);
		sqlite_sram_configure_slot(slot, img, nullptr);
		return false;
	}
	snprintf(img->path, sizeof(img->path), "%s", tmp_path);

	if (!sqlite_sram_run_db_migrations(g_slots[slot].db_path))
	{
		fprintf(stderr, "SQLite SRAM warning: DB migration check failed for \"%s\".\n", save_path);
	}

	if (!sqlite_sram_migrate_legacy_save(save_path, g_slots[slot].db_path))
	{
		fprintf(stderr, "SQLite SRAM warning: legacy save migration failed for \"%s\".\n", save_path);
	}

	std::vector<uint8_t> latest;
	bool found = false;
	if (!sqlite_sram_load_latest(g_slots[slot].db_path, latest, &found))
	{
		fprintf(stderr, "SQLite SRAM warning: failed to load latest snapshot for \"%s\".\n", save_path);
	}

	const bool have_snapshot = found && !latest.empty();
	if (have_snapshot)
	{
		if (!sqlite_sram_write_image(img, latest.data(), latest.size()))
		{
			fprintf(stderr, "SQLite SRAM error: failed to restore snapshot for \"%s\".\n", save_path);
			FileClose(img);
			sqlite_sram_configure_slot(slot, img, nullptr);
			return false;
		}

		if (pre_size > (int)latest.size())
		{
			if (!FileSeek(img, latest.size(), SEEK_SET))
			{
				FileClose(img);
				sqlite_sram_configure_slot(slot, img, nullptr);
				return false;
			}

			static uint8_t ff_buf[4096] = {};
			static bool ff_init = false;
			if (!ff_init)
			{
				memset(ff_buf, 0xFF, sizeof(ff_buf));
				ff_init = true;
			}

			int remaining = pre_size - (int)latest.size();
			while (remaining > 0)
			{
				int chunk = (remaining > (int)sizeof(ff_buf)) ? (int)sizeof(ff_buf) : remaining;
				if (FileWriteAdv(img, ff_buf, chunk, -1) != chunk)
				{
					FileClose(img);
					sqlite_sram_configure_slot(slot, img, nullptr);
					return false;
				}
				remaining -= chunk;
			}

			FileSeek(img, 0, SEEK_SET);
		}

		fprintf(stderr, "SQLite SRAM loaded: %s (%d bytes)\n", save_path, (int)latest.size());
	}
	else if (pre_size > 0)
	{
		if (!sqlite_sram_fill_ff(img, pre_size))
		{
			FileClose(img);
			sqlite_sram_configure_slot(slot, img, nullptr);
			return false;
		}
	}
	else
	{
		img->type = 2;
	}

	return true;
#else
	(void)slot;
	(void)save_path;
	(void)pre_size;
	(void)img;
	return false;
#endif
}

void sqlite_sram_unmount_slot(uint8_t slot)
{
#if SQLITE_SRAM_SNAPSHOTS
	if (slot >= SQLITE_SRAM_MAX_SLOTS) return;
	sqlite_sram_configure_slot(slot, nullptr, nullptr);
#else
	(void)slot;
#endif
}

void sqlite_sram_mark_save_dirty(uint8_t slot)
{
#if SQLITE_SRAM_SNAPSHOTS
	if (!sqlite_sram_runtime_enabled()) return;
	if (slot >= SQLITE_SRAM_MAX_SLOTS) return;

	sqlite_sram_slot_t &state = g_slots[slot];
	if (!state.enabled) return;

	state.dirty = true;
	if (!state.flush_timer) state.flush_timer = GetTimer(sqlite_sram_interval_ms());
#else
	(void)slot;
#endif
}

void sqlite_sram_flush_slot(uint8_t slot)
{
#if SQLITE_SRAM_SNAPSHOTS
	if (!sqlite_sram_runtime_enabled()) return;
	if (slot >= SQLITE_SRAM_MAX_SLOTS) return;
	sqlite_sram_try_flush(slot);
#else
	(void)slot;
#endif
}

void sqlite_sram_poll()
{
#if SQLITE_SRAM_SNAPSHOTS
	if (!sqlite_sram_runtime_enabled()) return;
	sqlite_sram_poll_export_trigger();
	sqlite_sram_poll_flush();
#endif
}
