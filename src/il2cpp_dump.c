/**
 * @file il2cpp_dump.c
 * @brief IL2CPP type enumeration and C#‑like dump routines.
 *
 * This module contains the core logic for walking IL2CPP metadata and
 * writing a human‑readable, C#‑like representation of all managed types
 * to a file.  It supports three strategies, tried in order:
 *
 *   1. **image_get_class** – iterate assemblies → images → classes via
 *      `il2cpp_image_get_class`.  This is the fastest path when
 *      available.
 *   2. **class_for_each** – use `il2cpp_class_for_each` callback (available
 *      in newer IL2CPP builds).
 *   3. **reflection** – load each assembly via `System.Reflection.Assembly`,
 *      call `GetTypes()`, and dump the returned classes.  This is the
 *      slowest but most compatible fallback.
 *
 * @section output Output format
 * The resulting `dump.cs` file is NOT valid C#; it is a pseudo‑source
 * listing aimed at reverse engineers.  Each class is annotated with:
 *   - Assembly name (`// Dll: Assembly‑CSharp`).
 *   - Namespace.
 *   - Attribute flags (visibility, abstract/sealed, Serializabe, etc.).
 *   - Fields with offset comment.
 *   - Properties with get/set accessors.
 *   - Methods with RVA/VA and parameter in/out/ref annotations.
 */

#include "il2cpp_dump.h"
#include "il2cpp_api.h"
#include "il2cpp_tabledefs.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Safely obtain a class name.
 *
 * Returns `<null_class>` if the class pointer is NULL,
 * `<no_api>` if the name API is missing, or `<unnamed>` if the name itself
 * is NULL.
 *
 * @param klass IL2CPP class pointer.
 * @return Never‑NULL string.
 */
static const char *safe_class_name(Il2CppClass *klass) {
    if (!klass) return "<null_class>";
    if (!il2cpp_class_get_name) return "<no_api>";
    const char *n = il2cpp_class_get_name(klass);
    return n ? n : "<unnamed>";
}

/**
 * @brief Determine whether a type is passed by reference (`ref`).
 *
 * Falls back to reading the `byref` field directly if the API function
 * is not available.
 *
 * @param type IL2CPP type descriptor.
 * @return true if the type is a by‑reference type.
 */
static bool type_is_byref(const Il2CppType *type) {
    if (!type) return false;
    if (il2cpp_type_is_byref) {
        return il2cpp_type_is_byref(type);
    }
    return (bool)type->byref;
}

/* -------------------------------------------------------------------------
 * Attribute printing helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Write C# access and method modifiers to the output file.
 *
 * Interprets the IL method flags (visibility, static, abstract, virtual,
 * override, sealed, extern) and prints the corresponding C# keywords.
 *
 * @param fp    Output file.
 * @param flags Raw method flags from IL2CPP metadata.
 */
static void write_method_modifier(FILE *fp, uint32_t flags) {
    uint32_t access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;

    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:      fprintf(fp, "private ");            break;
        case METHOD_ATTRIBUTE_PUBLIC:       fprintf(fp, "public ");             break;
        case METHOD_ATTRIBUTE_FAMILY:       fprintf(fp, "protected ");          break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM: fprintf(fp, "internal ");          break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM: fprintf(fp, "protected internal "); break;
        default: break;
    }

    if (flags & METHOD_ATTRIBUTE_STATIC) {
        fprintf(fp, "static ");
    }

    /* Abstract / override / virtual logic */
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        fprintf(fp, "abstract ");
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            fprintf(fp, "override ");
        }
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            fprintf(fp, "sealed override ");
        }
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT) {
            fprintf(fp, "virtual ");
        } else {
            fprintf(fp, "override ");
        }
    }

    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
        fprintf(fp, "extern ");
    }
}

/**
 * @brief Write field access modifiers and flags (static, readonly, const).
 *
 * Also prints the field type, name, and memory offset.  For literal fields
 * of an enum type, it writes the concrete constant value.
 *
 * @param fp    Output file.
 * @param klass The enclosing class (used to detect enums).
 */
static void dump_fields(FILE *fp, Il2CppClass *klass) {
    bool is_enum = il2cpp_class_is_enum ? il2cpp_class_is_enum(klass) : false;
    void *iter   = NULL;
    FieldInfo *field;

    fprintf(fp, "\n\t// Fields\n");

    while ((field = il2cpp_class_get_fields(klass, &iter)) != NULL) {
        int attrs  = il2cpp_field_get_flags(field);
        int access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;

        fprintf(fp, "\t");

        /* Visibility */
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:       fprintf(fp, "private ");            break;
            case FIELD_ATTRIBUTE_PUBLIC:        fprintf(fp, "public ");             break;
            case FIELD_ATTRIBUTE_FAMILY:        fprintf(fp, "protected ");          break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM: fprintf(fp, "internal ");           break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:  fprintf(fp, "protected internal "); break;
            default: break;
        }

        /* Static / const / readonly */
        if (attrs & FIELD_ATTRIBUTE_LITERAL) {
            fprintf(fp, "const ");
        } else {
            if (attrs & FIELD_ATTRIBUTE_STATIC)    fprintf(fp, "static ");
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) fprintf(fp, "readonly ");
        }

        /* Type and name */
        const Il2CppType *ftype  = il2cpp_field_get_type(field);
        Il2CppClass      *fclass = ftype ? il2cpp_class_from_type(ftype) : NULL;
        const char       *fname  = il2cpp_field_get_name(field);

        fprintf(fp, "%s %s",
                safe_class_name(fclass),
                fname ? fname : "<unnamed>");

        /* For literal enum fields, show the numeric value */
        if ((attrs & FIELD_ATTRIBUTE_LITERAL) && is_enum
                && il2cpp_field_static_get_value) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            fprintf(fp, " = %llu", (unsigned long long)val);
        }

        /* Always append the memory offset as a comment */
        size_t offset = il2cpp_field_get_offset(field);
        fprintf(fp, "; // 0x%zx\n", offset);
    }
}

/**
 * @brief Write property declarations.
 *
 * Determines the property type from the getter's return type or the
 * setter's first parameter, then prints the property with `get;` / `set;`
 * accessor placeholders.
 *
 * @param fp    Output file.
 * @param klass Enclosing class.
 */
static void dump_properties(FILE *fp, Il2CppClass *klass) {
    void *iter = NULL;
    const PropertyInfo *prop;

    fprintf(fp, "\n\t// Properties\n");

    while ((prop = il2cpp_class_get_properties(klass, &iter)) != NULL) {
        /* We cast away const because the API expects non‑const PropertyInfo* */
        PropertyInfo      *p   = (PropertyInfo *)prop;
        const MethodInfo  *get = il2cpp_property_get_get_method(p);
        const MethodInfo  *set = il2cpp_property_get_set_method(p);
        const char        *pname = il2cpp_property_get_name(p);

        fprintf(fp, "\t");

        Il2CppClass *prop_class = NULL;
        uint32_t     iflags     = 0;
        uint32_t     flags      = 0;

        if (get) {
            flags      = il2cpp_method_get_flags(get, &iflags);
            const Il2CppType *rtype = il2cpp_method_get_return_type(get);
            if (rtype) prop_class = il2cpp_class_from_type(rtype);
        } else if (set) {
            flags = il2cpp_method_get_flags(set, &iflags);
            if (il2cpp_method_get_param_count(set) > 0) {
                const Il2CppType *ptype = il2cpp_method_get_param(set, 0);
                if (ptype) prop_class = il2cpp_class_from_type(ptype);
            }
        }

        if (!prop_class) {
            if (pname) fprintf(fp, "// unknown property %s\n", pname);
            continue;
        }

        /* Use the same modifier logic as methods */
        write_method_modifier(fp, flags);
        fprintf(fp, "%s %s { ",
                safe_class_name(prop_class),
                pname ? pname : "<unnamed>");
        if (get) fprintf(fp, "get; ");
        if (set) fprintf(fp, "set; ");
        fprintf(fp, "}\n");
    }
}

/**
 * @brief Write method declarations.
 *
 * Includes the RVA (relative virtual address) and VA (absolute) as
 * a comment, then prints the method signature: modifiers, return type,
 * name, and parameter list (`ref`, `out`, `in`, `[In]`, `[Out]`).
 *
 * @param fp    Output file.
 * @param klass Enclosing class.
 * @param base  Base address of libil2cpp.so (for RVA calculation).
 */
static void dump_methods(FILE *fp, Il2CppClass *klass, uintptr_t base) {
    void *iter = NULL;
    const MethodInfo *method;

    fprintf(fp, "\n\t// Methods\n");

    while ((method = il2cpp_class_get_methods(klass, &iter)) != NULL) {
        /* Address comment */
        if (method->methodPointer) {
            uintptr_t va  = (uintptr_t)method->methodPointer;
            uintptr_t rva = (base && va > base) ? (va - base) : va;
            fprintf(fp, "\t// RVA: 0x%lx VA: 0x%lx\n",
                    (unsigned long)rva, (unsigned long)va);
        } else {
            fprintf(fp, "\t// RVA: 0x0 VA: 0x0\n");
        }

        fprintf(fp, "\t");

        /* Modifiers */
        uint32_t iflags = 0;
        uint32_t flags  = il2cpp_method_get_flags(method, &iflags);
        write_method_modifier(fp, flags);

        /* Return type */
        const Il2CppType *rtype  = il2cpp_method_get_return_type(method);
        Il2CppClass      *rclass = rtype ? il2cpp_class_from_type(rtype) : NULL;
        if (rtype && type_is_byref(rtype)) fprintf(fp, "ref ");
        fprintf(fp, "%s ", safe_class_name(rclass));

        /* Method name */
        const char *mname = il2cpp_method_get_name(method);
        fprintf(fp, "%s(", mname ? mname : "<unnamed>");

        /* Parameters */
        uint32_t param_count = il2cpp_method_get_param_count(method);
        for (uint32_t i = 0; i < param_count; ++i) {
            const Il2CppType *param  = il2cpp_method_get_param(method, i);
            Il2CppClass      *pclass = param ? il2cpp_class_from_type(param) : NULL;

            /* By‑ref handling with In/Out annotations */
            if (param && type_is_byref(param)) {
                uint32_t pattrs = param->attrs;
                bool     is_out = (pattrs & PARAM_ATTRIBUTE_OUT) && !(pattrs & PARAM_ATTRIBUTE_IN);
                bool     is_in  = (pattrs & PARAM_ATTRIBUTE_IN)  && !(pattrs & PARAM_ATTRIBUTE_OUT);
                if (is_out)       fprintf(fp, "out ");
                else if (is_in)   fprintf(fp, "in ");
                else              fprintf(fp, "ref ");
            } else if (param) {
                uint32_t pattrs = param->attrs;
                if (pattrs & PARAM_ATTRIBUTE_IN)  fprintf(fp, "[In] ");
                if (pattrs & PARAM_ATTRIBUTE_OUT) fprintf(fp, "[Out] ");
            }

            const char *pname = il2cpp_method_get_param_name
                                 ? il2cpp_method_get_param_name(method, i)
                                 : NULL;
            fprintf(fp, "%s %s",
                    safe_class_name(pclass),
                    pname ? pname : "param");

            if (i + 1 < param_count) fprintf(fp, ", ");
        }

        fprintf(fp, ") { }\n");
    }
}

/* -------------------------------------------------------------------------
 * Single class dump
 * ------------------------------------------------------------------------- */
/**
 * @brief Write the C#‑like declaration of a single IL2CPP class.
 *
 * The output includes:
 *   - Namespace comment.
 *   - Attributes (visibility, Serializable, abstract/sealed/static, etc.).
 *   - Kind (class, struct, enum, interface).
 *   - Base type and implemented interfaces.
 *   - Fields, properties, and methods.
 *
 * @param fp    Output file.
 * @param klass IL2CPP class to dump.
 * @param base  libil2cpp.so base address for RVA calculation.
 */
static void dump_single_class(FILE *fp, Il2CppClass *klass, uintptr_t base) {
    if (!klass || !il2cpp_class_get_type) return;

    const Il2CppType *type = il2cpp_class_get_type(klass);
    if (!type) return;

    /* Namespace comment */
    const char *ns = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(klass) : "";
    fprintf(fp, "\n// Namespace: %s\n", ns ? ns : "");

    int  flags       = il2cpp_class_get_flags  ? il2cpp_class_get_flags(klass)      : 0;
    bool is_valuetype= il2cpp_class_is_valuetype? il2cpp_class_is_valuetype(klass)  : false;
    bool is_enum     = il2cpp_class_is_enum     ? il2cpp_class_is_enum(klass)       : false;

    /* Serializable attribute */
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) fprintf(fp, "[Serializable]\n");

    /* Visibility */
    int visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:       fprintf(fp, "public ");            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:     fprintf(fp, "internal ");          break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:      fprintf(fp, "private ");           break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:       fprintf(fp, "protected ");         break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM: fprintf(fp, "protected internal "); break;
        default: break;
    }

    /* Abstract / sealed / static */
    bool is_abstract  = (flags & TYPE_ATTRIBUTE_ABSTRACT) != 0;
    bool is_sealed    = (flags & TYPE_ATTRIBUTE_SEALED) != 0;
    bool is_interface = (flags & TYPE_ATTRIBUTE_INTERFACE) != 0;

    if (is_abstract && is_sealed) {
        fprintf(fp, "static ");
    } else if (!is_interface && is_abstract) {
        fprintf(fp, "abstract ");
    } else if (!is_valuetype && !is_enum && is_sealed) {
        fprintf(fp, "sealed ");
    }

    /* Kind */
    if (is_interface)   fprintf(fp, "interface ");
    else if (is_enum)   fprintf(fp, "enum ");
    else if (is_valuetype) fprintf(fp, "struct ");
    else                fprintf(fp, "class ");

    /* Name */
    fprintf(fp, "%s", safe_class_name(klass));

    /* Base class and interfaces */
    {
        int  first_extend = 1;              /* unused, kept for clarity */
        char sep[4]       = " : ";

        /* Base type (skip System.Object) */
        if (!is_valuetype && !is_enum && il2cpp_class_get_parent && il2cpp_class_get_type) {
            Il2CppClass *parent = il2cpp_class_get_parent(klass);
            if (parent) {
                const Il2CppType *pt = il2cpp_class_get_type(parent);
                if (pt && pt->type != IL2CPP_TYPE_OBJECT) {
                    fprintf(fp, "%s%s", sep, safe_class_name(parent));
                    strcpy(sep, ", ");
                    first_extend = 0;
                }
            }
        }
        (void)first_extend; /* suppress unused warning */

        /* Interfaces */
        if (il2cpp_class_get_interfaces) {
            void *iter  = NULL;
            Il2CppClass *itf;
            while ((itf = il2cpp_class_get_interfaces(klass, &iter)) != NULL) {
                fprintf(fp, "%s%s", sep, safe_class_name(itf));
                strcpy(sep, ", ");
            }
        }
    }

    fprintf(fp, "\n{\n");
    dump_fields(fp, klass);
    dump_properties(fp, klass);
    dump_methods(fp, klass, base);
    fprintf(fp, "}\n");
}

/* -------------------------------------------------------------------------
 * Strategy 1: Dump via image_get_class
 * ------------------------------------------------------------------------- */
/**
 * @brief Dump all classes by iterating assemblies → images → classes.
 *
 * Requires `il2cpp_image_get_class` and `il2cpp_image_get_class_count`.
 * Each class is annotated with the DLL (image) name.
 *
 * @param fp   Output file.
 * @param base libil2cpp.so base address.
 * @return 1 on success, 0 if the required APIs are missing.
 */
static int dump_by_image(FILE *fp, uintptr_t base) {
    if (!il2cpp_image_get_class || !il2cpp_image_get_class_count) {
        LOGI("il2cpp_image_get_class not available; trying fallback");
        return 0;
    }

    Il2CppDomain        *domain    = il2cpp_domain_get();
    size_t               asm_count = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &asm_count);

    if (!assemblies || asm_count == 0) {
        LOGE("No assemblies found in domain");
        return 0;
    }

    LOGI("Dumping %zu assemblies (strategy: image_get_class)", asm_count);

    /* First pass: list assemblies for readability */
    size_t i;
    for (i = 0; i < asm_count; ++i) {
        const Il2CppImage *img  = il2cpp_assembly_get_image(assemblies[i]);
        const char        *name = img ? il2cpp_image_get_name(img) : "?";
        fprintf(fp, "// Image %zu: %s\n", i, name ? name : "?");
    }

    /* Second pass: dump classes grouped by assembly */
    for (i = 0; i < asm_count; ++i) {
        const Il2CppImage *img  = il2cpp_assembly_get_image(assemblies[i]);
        if (!img) continue;

        const char *img_name = il2cpp_image_get_name(img);
        size_t      cnt      = il2cpp_image_get_class_count(img);

        size_t j;
        for (j = 0; j < cnt; ++j) {
            const Il2CppClass *klass = il2cpp_image_get_class(img, j);
            if (!klass) continue;

            /* The callback expects a non‑const pointer, which is safe
               because dump_single_class only reads metadata. */
            Il2CppClass *k = (Il2CppClass *)klass;

            fprintf(fp, "\n// Dll: %s", img_name ? img_name : "?");
            dump_single_class(fp, k, base);
        }
    }

    return 1;
}

/* -------------------------------------------------------------------------
 * Strategy 2: Dump via il2cpp_class_for_each
 * ------------------------------------------------------------------------- */
/**
 * @brief Callback context for `dump_by_for_each`.
 */
typedef struct {
    FILE    *fp;      /**< Output file. */
    uintptr_t base;   /**< libil2cpp.so base address. */
    size_t   count;   /**< Number of classes written (accumulated). */
} ForEachCtx;

/**
 * @brief Callback invoked by `il2cpp_class_for_each`.
 *
 * Writes the class declaration with the DLL name obtained from
 * `il2cpp_class_get_image`.
 *
 * @param klass     Current class.
 * @param user_data Pointer to a ForEachCtx structure.
 */
static void for_each_callback(Il2CppClass *klass, void *user_data) {
    ForEachCtx *ctx = (ForEachCtx *)user_data;

    /* Determine DLL / image name */
    const char *dll = "?";
    if (il2cpp_class_get_image) {
        const Il2CppImage *img = il2cpp_class_get_image(klass);
        if (img && il2cpp_image_get_name) {
            const char *n = il2cpp_image_get_name((Il2CppImage*)img);
            if (n) dll = n;
        }
    }

    fprintf(ctx->fp, "\n// Dll: %s", dll);
    dump_single_class(ctx->fp, klass, ctx->base);
    ctx->count++;
}

/**
 * @brief Dump all classes using `il2cpp_class_for_each`.
 *
 * Requires the `il2cpp_class_for_each` function pointer.
 *
 * @param fp   Output file.
 * @param base libil2cpp.so base address.
 * @return 1 if at least one class was written, 0 otherwise.
 */
static int dump_by_for_each(FILE *fp, uintptr_t base) {
    if (!il2cpp_class_for_each) return 0;

    LOGI("Dumping via il2cpp_class_for_each (strategy: for_each)");

    ForEachCtx ctx;
    ctx.fp    = fp;
    ctx.base  = base;
    ctx.count = 0;

    il2cpp_class_for_each(for_each_callback, &ctx);
    LOGI("il2cpp_class_for_each enumerated %zu classes", ctx.count);
    return (ctx.count > 0) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Strategy 3: Reflection‑based dump (slow fallback)
 * ------------------------------------------------------------------------- */
/** Prototype for Assembly.Load(string) */
typedef void *(*AssemblyLoad_ftn)(void *, Il2CppString *, void *);
/** Prototype for Assembly.GetTypes() */
typedef Il2CppArray *(*AssemblyGetTypes_ftn)(void *, void *);

/**
 * @brief Dump all classes using .NET reflection APIs.
 *
 * This strategy requires `il2cpp_get_corlib` and
 * `il2cpp_class_from_system_type`.  It loads each assembly by name via
 * `Assembly.Load`, calls `GetTypes()`, and dumps each returned
 * `System.Type` by converting it back to an `Il2CppClass`.
 *
 * @param fp   Output file.
 * @param base libil2cpp.so base address.
 * @return 1 on success, 0 if necessary APIs are missing.
 */
static int dump_by_reflection(FILE *fp, uintptr_t base) {
    if (!il2cpp_get_corlib || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name || !il2cpp_string_new ||
        !il2cpp_class_from_system_type) {
        LOGW("Reflection APIs unavailable; cannot fall back to GetTypes()");
        return 0;
    }

    const Il2CppImage *corlib      = il2cpp_get_corlib();
    Il2CppClass       *asm_class   = il2cpp_class_from_name(
                                         corlib, "System.Reflection", "Assembly");
    if (!asm_class) { LOGE("Cannot find System.Reflection.Assembly"); return 0; }

    const MethodInfo  *load_method = il2cpp_class_get_method_from_name(asm_class, "Load", 1);
    const MethodInfo  *types_method= il2cpp_class_get_method_from_name(asm_class, "GetTypes", 0);

    if (!load_method  || !load_method->methodPointer)  { LOGE("Assembly.Load not found");     return 0; }
    if (!types_method || !types_method->methodPointer) { LOGE("Assembly.GetTypes not found"); return 0; }

    AssemblyLoad_ftn     fn_load  = (AssemblyLoad_ftn)    load_method->methodPointer;
    AssemblyGetTypes_ftn fn_types = (AssemblyGetTypes_ftn)types_method->methodPointer;

    Il2CppDomain        *domain     = il2cpp_domain_get();
    size_t               asm_count  = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &asm_count);

    if (!assemblies || asm_count == 0) {
        LOGE("No assemblies for reflection dump");
        return 0;
    }

    LOGI("Reflection dump: %zu assemblies", asm_count);

    size_t i;
    for (i = 0; i < asm_count; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        if (!img) continue;

        const char *img_name = il2cpp_image_get_name(img);
        if (!img_name) img_name = "?";

        /* Strip .dll extension to get the assembly name for Load() */
        char asm_name[256];
        strncpy(asm_name, img_name, sizeof(asm_name) - 1);
        asm_name[sizeof(asm_name) - 1] = '\0';
        char *dot = strrchr(asm_name, '.');
        if (dot) *dot = '\0';

        Il2CppString *asm_str = il2cpp_string_new(asm_name);
        void         *refl_asm = fn_load(NULL, asm_str, NULL);
        if (!refl_asm) {
            LOGW("Assembly.Load failed for: %s", asm_name);
            continue;
        }

        Il2CppArray *types = fn_types(refl_asm, NULL);
        if (!types) continue;

        il2cpp_array_size_t j;
        for (j = 0; j < types->max_length; ++j) {
            Il2CppReflectionType *rt    = (Il2CppReflectionType *)types->vector[j];
            Il2CppClass          *klass = il2cpp_class_from_system_type(rt);
            if (!klass) continue;

            fprintf(fp, "\n// Dll: %s", img_name);
            dump_single_class(fp, klass, base);
        }
    }

    return 1;
}

/* -------------------------------------------------------------------------
 * Public API: initialisation helper & main dump entry
 * ------------------------------------------------------------------------- */
/**
 * @brief Block until the IL2CPP domain is available.
 *
 * Polls `il2cpp_domain_get` every second for up to 60 seconds.
 * If the function pointer itself is NULL, we cannot perform the check
 * and simply wait for 3 seconds before returning success.
 *
 * @return 1 if the domain is ready, 0 on timeout.
 */
int il2cpp_wait_for_init(void) {
    LOGI("Waiting for il2cpp domain to become ready...");

    /* If the pointer is missing we cannot wait; assume it's ok after
       a short delay. */
    if (!il2cpp_domain_get) {
        LOGW("il2cpp_domain_get unavailable; skipping init check");
        sleep(3);
        return 1;
    }

    int timeout = 60;
    while (timeout-- > 0) {
        Il2CppDomain *d = il2cpp_domain_get();
        if (d != NULL) {
            LOGI("il2cpp domain ready (%p)", (void *)d);
            return 1;
        }
        sleep(1);
    }

    LOGE("Timed out waiting for il2cpp domain");
    return 0;
}

/**
 * @brief Perform the IL2CPP metadata dump.
 *
 * Opens the output file (retrying up to 10 times if the directory
 * does not exist, and falling back to alternative paths on failure),
 * determines the base address of libil2cpp.so, and tries the three
 * dumping strategies in order.
 *
 * @param output_path Desired path for the dump file (e.g. /.../dump.cs).
 * @return 1 on success, 0 on failure.
 */
int il2cpp_do_dump(const char *output_path) {
    if (!output_path) { LOGE("output_path is NULL"); return 0; }

    /* -----------------------------------------------------------------
     * Retry‑based file opening with directory creation
     * ----------------------------------------------------------------- */
    FILE *fp = NULL;
    int   attempt;

    for (attempt = 1; attempt <= 10; ++attempt) {
        /* Ensure the directory exists before each attempt */
        {
            char dir[512];
            strncpy(dir, output_path, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            char *slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                /* mkdir -p loop */
                char tmp[512];
                strncpy(tmp, dir, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                char *p;
                for (p = tmp + 1; *p; ++p) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(tmp, 0755);
                        *p = '/';
                    }
                }
                mkdir(tmp, 0755);
            }
        }

        fp = fopen(output_path, "w");
        if (fp) {
            LOGI("Output file opened on attempt %d: %s", attempt, output_path);
            break;
        }

        LOGW("fopen attempt %d/10 failed: %s  errno=%d (%s)",
             attempt, output_path, errno, strerror(errno));

        if (attempt < 10) sleep(1);
    }

    /* -----------------------------------------------------------------
     * Fallback paths when the primary path cannot be opened
     * ----------------------------------------------------------------- */
    if (!fp) {
        /* Try to extract package name from the original path
           (expected pattern: /data/data/<pkg>/...) */
        char pkg[256] = {0};
        const char *p = strstr(output_path, "/data/data/");
        if (p) {
            p += strlen("/data/data/");
            const char *slash = strchr(p, '/');
            size_t      len   = slash ? (size_t)(slash - p) : strlen(p);
            if (len < sizeof(pkg)) {
                memcpy(pkg, p, len);
                pkg[len] = '\0';
            }
        }

        /* Prepare a list of fallback directories */
        const char *fallback_dirs[4];
        char fb0[512], fb1[512], fb2[512];
        int  nfb = 0;

        if (pkg[0]) {
            snprintf(fb0, sizeof(fb0),
                     "/sdcard/Android/data/%s/files/il2cpp_dump", pkg);
            snprintf(fb1, sizeof(fb1),
                     "/storage/emulated/0/Android/data/%s/files/il2cpp_dump",
                     pkg);
            fallback_dirs[nfb++] = fb0;
            fallback_dirs[nfb++] = fb1;
        }
        snprintf(fb2, sizeof(fb2), "/sdcard/il2cpp_dump");
        fallback_dirs[nfb++] = fb2;
        fallback_dirs[nfb]   = NULL;

        int fi;
        for (fi = 0; fi < nfb && !fp; ++fi) {
            /* mkdir -p for fallback directory */
            {
                char tmp[512];
                strncpy(tmp, fallback_dirs[fi], sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                char *q;
                for (q = tmp + 1; *q; ++q) {
                    if (*q == '/') { *q = '\0'; mkdir(tmp, 0755); *q = '/'; }
                }
                mkdir(tmp, 0755);
            }

            char fb_file[640];
            snprintf(fb_file, sizeof(fb_file), "%s/dump.cs",
                     fallback_dirs[fi]);

            fp = fopen(fb_file, "w");
            if (fp) {
                LOGI("Using fallback output path: %s", fb_file);
            } else {
                LOGW("Fallback also failed: %s  errno=%d (%s)",
                     fb_file, errno, strerror(errno));
            }
        }
    }

    if (!fp) {
        LOGE("All output paths failed. Dump aborted.");
        return 0;
    }

    /* -----------------------------------------------------------------
     * Obtain IL2CPP base address
     * ----------------------------------------------------------------- */
    uintptr_t base = il2cpp_get_base();
    LOGI("il2cpp base: 0x%lx", (unsigned long)base);

    /* -----------------------------------------------------------------
     * Try dump strategies in order of preference
     * ----------------------------------------------------------------- */
    int ok = 0;

    /* Strategy 1: Image‑based enumeration (fastest, well‑structured) */
    if (!ok) ok = dump_by_image(fp, base);
    /* Strategy 2: For‑each callback (newer IL2CPP builds) */
    if (!ok) ok = dump_by_for_each(fp, base);
    /* Strategy 3: Reflection fallback (slow, but very compatible) */
    if (!ok) ok = dump_by_reflection(fp, base);

    if (!ok) {
        /* Record which APIs were missing for post‑mortem debugging */
        fprintf(fp, "// ERROR: No suitable enumeration strategy found.\n");
        fprintf(fp, "// il2cpp_image_get_class: %s\n",
                il2cpp_image_get_class ? "available" : "missing");
        fprintf(fp, "// il2cpp_class_for_each:  %s\n",
                il2cpp_class_for_each  ? "available" : "missing");
        LOGE("All dump strategies failed");
    }

    fflush(fp);
    fclose(fp);

    if (ok) LOGI("Dump complete -> %s", output_path);
    return ok;
}