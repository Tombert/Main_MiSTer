#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "usage: $0 <output-header>" >&2
	exit 1
fi

out_file="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sql_dir="$script_dir/migrations"

mkdir -p "$(dirname "$out_file")"
tmp_file="${out_file}.tmp"

shopt -s nullglob
files=("$sql_dir"/*.sql)
if (( ${#files[@]} )); then
	IFS=$'\n' sorted_files=($(printf '%s\n' "${files[@]}" | LC_ALL=C sort))
	unset IFS
else
	sorted_files=()
fi

{
	echo "#ifndef SQLITE_SRAM_MIGRATIONS_AUTOGEN_H"
	echo "#define SQLITE_SRAM_MIGRATIONS_AUTOGEN_H"
	echo
	echo "#include <stddef.h>"
	echo
	echo "#include \"support/sqlite_sram/migrations.h\""
	echo
	echo "const sqlite_sram_migration_t g_sqlite_sram_migrations[] = {"
	for file in "${sorted_files[@]}"; do
		name="$(basename "$file")"
		echo "	{\"$name\", R\"__MIG__("
		cat "$file"
		echo ")__MIG__\"},"
	done
	echo "};"
	echo
	echo "const size_t g_sqlite_sram_migrations_count = sizeof(g_sqlite_sram_migrations) / sizeof(g_sqlite_sram_migrations[0]);"
	echo
	echo "#endif"
} > "$tmp_file"

if cmp -s "$tmp_file" "$out_file" 2>/dev/null; then
	rm -f "$tmp_file"
else
	mv "$tmp_file" "$out_file"
fi
