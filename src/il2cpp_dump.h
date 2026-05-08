#ifndef IL2CPP_DUMPER_DUMP_H
#define IL2CPP_DUMPER_DUMP_H

/**
 * @file il2cpp_dump.h
 * @brief Public interface for the IL2CPP metadata dump.
 *
 * Declares the single entry point that triggers the full enumeration of
 * IL2CPP types and writes a C#‑like listing to the specified file.
 */

/**
 * @brief Execute the managed type dump.
 *
 * This function will:
 *   1. Open (or create) the output file at @p output_path, retrying on
 *      failure and falling back to alternative directories if necessary.
 *   2. Determine the base address of `libil2cpp.so`.
 *   3. Iterate all loaded managed assemblies and write a pseudo‑C#
 *      representation of every class, including fields, properties, and
 *      methods, into the file.
 *
 * The dump is performed on the calling thread.  The caller is responsible
 * for ensuring that the IL2CPP runtime is fully initialised and that the
 * API function pointers have been resolved (e.g. via
 * @ref il2cpp_api_init_ex ) before invoking this function.
 *
 * @param output_path  Absolute path for the output file (e.g.
 *                     `/data/data/com.example/files/il2cpp_dump/dump.cs`).
 * @return 1 on success, 0 on failure (missing APIs, no assemblies, I/O
 *         error, or all dump strategies failed).
 */
int il2cpp_do_dump(const char *output_path);

#endif