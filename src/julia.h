#ifndef JULIA_H
#define JULIA_H

#if (defined(_WIN32) || defined (_MSC_VER)) && !defined(__WIN32__)
    #define __WIN32__
#endif

#include "libsupport.h"
#include <stdint.h>
#include "uv.h"

#define JL_GC_MARKSWEEP

#include "htable.h"
#include "arraylist.h"

#include <setjmp.h>
#ifndef __WIN32__
#  define jl_jmp_buf sigjmp_buf
#else
#  define jl_jmp_buf jmp_buf
#  include <malloc.h> //for _resetstkoflw
#endif

#if __GNUC__
#define NORETURN __attribute__ ((noreturn))
#else
#define NORETURN
#endif

#ifdef _P64
// a risky way to save 8 bytes per tuple
//#define OVERLAP_TUPLE_LEN
#endif

#ifdef OVERLAP_TUPLE_LEN
#define JL_DATA_TYPE    \
    size_t type : 52;   \
    size_t _resvd : 12;
#else
#define JL_DATA_TYPE \
    struct _jl_value_t *type;
#endif

typedef struct _jl_value_t {
    JL_DATA_TYPE
} jl_value_t;

typedef struct _jl_sym_t {
    JL_DATA_TYPE
    struct _jl_sym_t *left;
    struct _jl_sym_t *right;
    uptrint_t hash;    // precomputed hash value
    union {
        char name[1];
        void *_pad;    // ensure field aligned to pointer size
    };
} jl_sym_t;

typedef struct {
#ifdef OVERLAP_TUPLE_LEN
    size_t type : 52;
    size_t length : 12;
#else
    JL_DATA_TYPE
    size_t length;
#endif
    jl_value_t *data[1];
} jl_tuple_t;

// pseudo-object to track managed malloc pointers
// currently only referenced from an array's data owner field
typedef struct _jl_mallocptr_t {
    struct _jl_mallocptr_t *next;
    size_t sz;
    void *ptr;
} jl_mallocptr_t;

// how much space we're willing to waste if an array outgrows its
// original object
#define ARRAY_INLINE_NBYTES (2048*sizeof(void*))

/*
  array data is allocated in two ways: either inline in the array object,
  (in _space), or with malloc with data owner pointing to a jl_mallocptr_t.
  data owner can also point to another array, if the original data was
  allocated inline.
*/
typedef struct {
    JL_DATA_TYPE
    void *data;
    size_t length;

    unsigned short ndims:14;
    unsigned short ptrarray:1;  // representation is pointer array
    unsigned short ismalloc:1;  // data owner is a jl_mallocptr_t
    uint16_t elsize;
    uint32_t offset;  // for 1-d only. does not need to get big.

    size_t nrows;
    union {
        // 1d
        size_t maxsize;
        // Nd
        size_t ncols;
    };
    // other dim sizes go here for ndims > 2

    union {
        char _space[1];
        void *_pad;
    };
} jl_array_t;

#define jl_array_len(a)   (((jl_array_t*)(a))->length)
#define jl_array_data(a)  ((void*)((jl_array_t*)(a))->data)
#define jl_array_dim(a,i) ((&((jl_array_t*)(a))->nrows)[i])
#define jl_array_dim0(a)  (((jl_array_t*)(a))->nrows)
#define jl_array_nrows(a) (((jl_array_t*)(a))->nrows)
#define jl_array_ndims(a) ((int32_t)(((jl_array_t*)a)->ndims))
#define jl_array_data_owner(a) (*((jl_value_t**)jl_array_inline_data_area(a)))

// compute # of extra words needed to store dimensions
static inline int jl_array_ndimwords(uint32_t ndims)
{
#ifdef _P64
    // on 64-bit, ndimwords must be even to give 16-byte alignment
    return (ndims == 0 ? 0 : ((ndims-1) & -2));
#else
    // on 32-bit, ndimwords must = 4k+1 to give 16-byte alignment
    return (ndims & -4) + 1;
#endif
}

static inline void *jl_array_inline_data_area(jl_array_t *a)
{
    return &a->_space[0] + jl_array_ndimwords(jl_array_ndims(a))*sizeof(size_t);
}

typedef jl_value_t *(*jl_fptr_t)(jl_value_t*, jl_value_t**, uint32_t);

typedef struct _jl_lambda_info_t {
    JL_DATA_TYPE
    // this holds the static data for a function:
    // a syntax tree, static parameters, and (if it has been compiled)
    // a function pointer.
    // this is the stuff that's shared among different instantiations
    // (different environments) of a closure.
    jl_value_t *ast;
    // sparams is a tuple (symbol, value, symbol, value, ...)
    jl_tuple_t *sparams;
    jl_value_t *tfunc;
    jl_sym_t *name;  // for error reporting
    jl_array_t *roots;  // pointers in generated code
    jl_tuple_t *specTypes;  // argument types this is specialized for
    // a slower-but-works version of this function as a fallback
    struct _jl_function_t *unspecialized;
    // array of all lambda infos with code generated from this one
    jl_array_t *specializations;
    struct _jl_module_t *module;
    struct _jl_lambda_info_t *def;  // original this is specialized from
    jl_value_t *capt;  // captured var info
    jl_sym_t *file;
    int32_t line;
    int8_t inferred;

    // hidden fields:
    // flag telling if inference is running on this function
    // used to avoid infinite recursion
    int8_t inInference : 1;
    int8_t inCompile : 1;
    jl_fptr_t fptr;        // jlcall entry point
    void *functionObject;  // jlcall llvm Function
    void *cFunctionObject; // c callable llvm Function
} jl_lambda_info_t;

#define LAMBDA_INFO_NW (NWORDS(sizeof(jl_lambda_info_t))-1)

#define JL_FUNC_FIELDS                          \
    jl_fptr_t fptr;                             \
    jl_value_t *env;                            \
    jl_lambda_info_t *linfo;

typedef struct _jl_function_t {
    JL_DATA_TYPE
    JL_FUNC_FIELDS
} jl_function_t;

typedef struct {
    JL_DATA_TYPE
    jl_tuple_t *parameters;
    jl_value_t *body;
} jl_typector_t;

typedef struct {
    JL_DATA_TYPE
    jl_sym_t *name;
    struct _jl_module_t *module;
    // if this is the name of a parametric type, this field points to the
    // original type.
    // a type alias, for example, might make a type constructor that is
    // not the original.
    jl_value_t *primary;
    jl_value_t *cache;
} jl_typename_t;

typedef struct {
    JL_DATA_TYPE
    jl_tuple_t *types;
} jl_uniontype_t;

typedef struct {
    uint16_t offset;   // offset relative to data start, excluding type tag
    uint16_t size:15;
    uint16_t isptr:1;
} jl_fielddesc_t;

typedef struct _jl_datatype_t {
    JL_DATA_TYPE
    JL_FUNC_FIELDS
    jl_typename_t *name;
    struct _jl_datatype_t *super;
    jl_tuple_t *parameters;
    jl_tuple_t *names;
    jl_tuple_t *types;
    // to create a set of constructors for this sort of type
    jl_value_t *ctor_factory;
    jl_value_t *instance;  // for singletons
    int32_t size;
    uint8_t abstract;
    uint8_t mutabl;
    uint8_t pointerfree;
    // hidden fields:
    uint32_t alignment;  // strictest alignment over all fields
    uint32_t uid;
    void *struct_decl;  //llvm::Value*
    jl_fielddesc_t fields[1];
} jl_datatype_t;

#define jl_field_offset(st,i) (((jl_datatype_t*)st)->fields[i].offset)
#define jl_field_size(st,i)   (((jl_datatype_t*)st)->fields[i].size)

typedef struct {
    JL_DATA_TYPE
    jl_sym_t *name;
    jl_value_t *lb;   // lower bound
    jl_value_t *ub;   // upper bound
    uptrint_t bound;  // part of a constraint environment
} jl_tvar_t;

typedef struct {
    JL_DATA_TYPE
    jl_value_t *value;
} jl_weakref_t;

typedef struct {
    // not first-class
    jl_sym_t *name;
    jl_value_t *value;
    jl_value_t *type;
    struct _jl_module_t *owner;  // for individual imported bindings
    unsigned constp:1;
    unsigned exportp:1;
    unsigned imported:1;
} jl_binding_t;

typedef struct _jl_callback_t {
    JL_DATA_TYPE
    jl_function_t *function;
    jl_tuple_t *types;
} jl_callback_t;

typedef struct _jl_module_t {
    JL_DATA_TYPE
    jl_sym_t *name;
    struct _jl_module_t *parent;
    htable_t bindings;
    arraylist_t usings;  // modules with all bindings potentially imported
} jl_module_t;

typedef struct _jl_methlist_t {
    JL_DATA_TYPE
    jl_tuple_t *sig;
    int8_t va;
    jl_tuple_t *tvars;
    jl_function_t *func;
    // cache of specializations of this method for invoke(), i.e.
    // cases where this method was called even though it was not necessarily
    // the most specific for the argument types.
    struct _jl_methtable_t *invokes;
    // TODO: pointer from specialized to original method
    //jl_function_t *orig_method;
    struct _jl_methlist_t *next;
} jl_methlist_t;

//#define JL_GF_PROFILE

typedef struct _jl_methtable_t {
    JL_DATA_TYPE
    jl_sym_t *name;
    jl_methlist_t *defs;
    jl_methlist_t *cache;
    jl_array_t *cache_arg1;
    jl_array_t *cache_targ;
    ptrint_t max_args;  // max # of non-vararg arguments in a signature
#ifdef JL_GF_PROFILE
    int ncalls;
#endif
} jl_methtable_t;

typedef struct {
    JL_DATA_TYPE
    jl_sym_t *head;
    jl_array_t *args;
    jl_value_t *etype;
} jl_expr_t;

enum CALLBACK_TYPE { CB_PTR, CB_INT32, CB_INT64 };
#ifdef _P64
#define CB_INT CB_INT64
#else
#define CB_INT CB_INT32
#endif

extern jl_datatype_t *jl_any_type;
extern jl_datatype_t *jl_type_type;
extern jl_tvar_t     *jl_typetype_tvar;
extern jl_datatype_t *jl_typetype_type;
extern jl_value_t    *jl_ANY_flag;
extern jl_datatype_t *jl_undef_type;
extern jl_datatype_t *jl_typename_type;
extern jl_datatype_t *jl_typector_type;
extern jl_datatype_t *jl_sym_type;
extern jl_datatype_t *jl_symbol_type;
extern jl_tuple_t *jl_tuple_type;
extern jl_datatype_t *jl_ntuple_type;
extern jl_typename_t *jl_ntuple_typename;
extern jl_datatype_t *jl_tvar_type;
extern jl_datatype_t *jl_task_type;

extern jl_datatype_t *jl_uniontype_type;
extern jl_datatype_t *jl_datatype_type;

extern jl_value_t *jl_bottom_type;
extern jl_value_t *jl_top_type;
extern jl_datatype_t *jl_lambda_info_type;
extern DLLEXPORT jl_datatype_t *jl_module_type;
extern jl_datatype_t *jl_vararg_type;
extern jl_datatype_t *jl_function_type;
extern jl_datatype_t *jl_abstractarray_type;
extern jl_datatype_t *jl_array_type;
extern jl_typename_t *jl_array_typename;
extern jl_datatype_t *jl_weakref_type;
extern DLLEXPORT jl_datatype_t *jl_ascii_string_type;
extern DLLEXPORT jl_datatype_t *jl_utf8_string_type;
extern DLLEXPORT jl_datatype_t *jl_errorexception_type;
extern DLLEXPORT jl_datatype_t *jl_loaderror_type;
extern jl_datatype_t *jl_typeerror_type;
extern jl_datatype_t *jl_methoderror_type;
extern jl_value_t *jl_stackovf_exception;
extern jl_value_t *jl_memory_exception;
extern jl_value_t *jl_divbyzero_exception;
extern jl_value_t *jl_domain_exception;
extern jl_value_t *jl_overflow_exception;
extern jl_value_t *jl_inexact_exception;
extern jl_value_t *jl_undefref_exception;
extern jl_value_t *jl_interrupt_exception;
extern jl_value_t *jl_bounds_exception;
extern jl_value_t *jl_an_empty_cell;

extern jl_datatype_t *jl_box_type;
extern jl_value_t *jl_box_any_type;
extern jl_typename_t *jl_box_typename;

extern jl_datatype_t *jl_bool_type;
extern jl_datatype_t *jl_char_type;
extern jl_datatype_t *jl_int8_type;
extern jl_datatype_t *jl_uint8_type;
extern jl_datatype_t *jl_int16_type;
extern jl_datatype_t *jl_uint16_type;
extern jl_datatype_t *jl_int32_type;
extern jl_datatype_t *jl_uint32_type;
extern jl_datatype_t *jl_int64_type;
extern jl_datatype_t *jl_uint64_type;
extern jl_datatype_t *jl_float32_type;
extern jl_datatype_t *jl_float64_type;
extern jl_datatype_t *jl_voidpointer_type;
extern jl_datatype_t *jl_pointer_type;

extern jl_value_t *jl_array_uint8_type;
extern jl_value_t *jl_array_any_type;
extern jl_value_t *jl_array_symbol_type;
extern DLLEXPORT jl_datatype_t *jl_expr_type;
extern jl_datatype_t *jl_symbolnode_type;
extern jl_datatype_t *jl_getfieldnode_type;
extern jl_datatype_t *jl_linenumbernode_type;
extern jl_datatype_t *jl_labelnode_type;
extern jl_datatype_t *jl_gotonode_type;
extern jl_datatype_t *jl_quotenode_type;
extern jl_datatype_t *jl_topnode_type;
extern jl_datatype_t *jl_intrinsic_type;
extern jl_datatype_t *jl_methtable_type;
extern jl_datatype_t *jl_method_type;
extern jl_datatype_t *jl_task_type;

extern jl_tuple_t *jl_null;
#define JL_NULL ((void*)jl_null)
extern jl_value_t *jl_true;
extern jl_value_t *jl_false;
DLLEXPORT extern jl_value_t *jl_nothing;

extern jl_function_t *jl_unprotect_stack_func;
extern jl_function_t *jl_bottom_func;

extern uv_lib_t *jl_dl_handle;
#if defined(__WIN32__) || defined (_WIN32)
extern uv_lib_t *jl_ntdll_handle;
extern uv_lib_t *jl_exe_handle;
extern uv_lib_t *jl_kernel32_handle;
extern uv_lib_t *jl_crtdll_handle;
extern uv_lib_t *jl_winsock_handle;
#endif
extern uv_loop_t *jl_io_loop;

// some important symbols
extern jl_sym_t *call_sym;
extern jl_sym_t *call1_sym;
extern jl_sym_t *dots_sym;
extern jl_sym_t *quote_sym;
extern jl_sym_t *top_sym;
extern jl_sym_t *line_sym;    extern jl_sym_t *toplevel_sym;
extern DLLEXPORT jl_sym_t *jl_continue_sym;
extern jl_sym_t *error_sym;   extern jl_sym_t *amp_sym;
extern jl_sym_t *module_sym;  extern jl_sym_t *colons_sym;
extern jl_sym_t *export_sym;  extern jl_sym_t *import_sym;
extern jl_sym_t *importall_sym; extern jl_sym_t *using_sym;
extern jl_sym_t *goto_sym;    extern jl_sym_t *goto_ifnot_sym;
extern jl_sym_t *label_sym;   extern jl_sym_t *return_sym;
extern jl_sym_t *lambda_sym;  extern jl_sym_t *assign_sym;
extern jl_sym_t *null_sym;    extern jl_sym_t *body_sym;
extern jl_sym_t *macro_sym;   extern jl_sym_t *method_sym;
extern jl_sym_t *enter_sym;   extern jl_sym_t *leave_sym;
extern jl_sym_t *exc_sym;     extern jl_sym_t *new_sym;
extern jl_sym_t *static_typeof_sym;
extern jl_sym_t *const_sym;   extern jl_sym_t *thunk_sym;
extern jl_sym_t *anonymous_sym;  extern jl_sym_t *underscore_sym;
extern jl_sym_t *abstracttype_sym; extern jl_sym_t *bitstype_sym;
extern jl_sym_t *compositetype_sym; extern jl_sym_t *type_goto_sym;
extern jl_sym_t *global_sym;  extern jl_sym_t *tuple_sym;


#ifdef _P64
#define NWORDS(sz) (((sz)+7)>>3)
#else
#define NWORDS(sz) (((sz)+3)>>2)
#endif

#ifdef JL_GC_MARKSWEEP
void *allocb(size_t sz);
void *allocobj(size_t sz);
#else
#define allocb(nb)    malloc(nb)
#define allocobj(nb)  malloc(nb)
#endif

#ifdef OVERLAP_TUPLE_LEN
#define jl_tupleref(t,i) (((jl_value_t**)(t))[1+(i)])
#define jl_tupleset(t,i,x) ((((jl_value_t**)(t))[1+(i)])=(jl_value_t*)(x))
#else
#define jl_tupleref(t,i) (((jl_value_t**)(t))[2+(i)])
#define jl_tupleset(t,i,x) ((((jl_value_t**)(t))[2+(i)])=(jl_value_t*)(x))
#endif
#define jl_t0(t) jl_tupleref(t,0)
#define jl_t1(t) jl_tupleref(t,1)

#define jl_cellref(a,i) (((jl_value_t**)((jl_array_t*)a)->data)[(i)])
#define jl_cellset(a,i,x) ((((jl_value_t**)((jl_array_t*)a)->data)[(i)])=((jl_value_t*)(x)))

#define jl_exprarg(e,n) jl_cellref(((jl_expr_t*)(e))->args,n)

#define jl_fieldref(s,i) jl_get_nth_field(((jl_value_t*)s),i)

#define jl_symbolnode_sym(s) ((jl_sym_t*)jl_fieldref(s,0))
#define jl_symbolnode_type(s) (jl_fieldref(s,1))
#define jl_linenode_line(x) (((ptrint_t*)x)[1])
#define jl_labelnode_label(x) (((ptrint_t*)x)[1])
#define jl_gotonode_label(x) (((ptrint_t*)x)[1])
#define jl_getfieldnode_val(s) (jl_fieldref(s,0))
#define jl_getfieldnode_name(s) ((jl_sym_t*)jl_fieldref(s,1))
#define jl_getfieldnode_type(s) (jl_fieldref(s,2))

#define jl_tparam0(t) jl_tupleref(((jl_datatype_t*)(t))->parameters, 0)
#define jl_tparam1(t) jl_tupleref(((jl_datatype_t*)(t))->parameters, 1)

#ifdef OVERLAP_TUPLE_LEN
#define jl_typeof(v) ((jl_value_t*)(uptrint_t)((jl_value_t*)(v))->type)
#else
#define jl_typeof(v) (((jl_value_t*)(v))->type)
#endif
#define jl_typeis(v,t) (jl_typeof(v)==(jl_value_t*)(t))

#define jl_is_null(v)        (((jl_value_t*)(v)) == ((jl_value_t*)jl_null))
#define jl_is_tuple(v)       jl_typeis(v,jl_tuple_type)
#define jl_is_datatype(v)    jl_typeis(v,jl_datatype_type)
#define jl_datatype_size(t)  (((jl_datatype_t*)t)->size)
#define jl_is_pointerfree(t) (((jl_datatype_t*)t)->pointerfree)
#define jl_ismutable(t)      (((jl_datatype_t*)t)->mutabl)
#define jl_is_mutable(t)     (((jl_datatype_t*)t)->mutabl)
#define jl_is_mutable_datatype(t) (jl_is_datatype(t) && (((jl_datatype_t*)t)->mutabl))
#define jl_isimmutable(t)    (!((jl_datatype_t*)t)->mutabl)
#define jl_is_immutable(t)   (!((jl_datatype_t*)t)->mutabl)
#define jl_is_immutable_datatype(t) (jl_is_datatype(t) && (!((jl_datatype_t*)t)->mutabl))
#define jl_is_uniontype(v)   jl_typeis(v,jl_uniontype_type)
#define jl_is_typevar(v)     jl_typeis(v,jl_tvar_type)
#define jl_is_typector(v)    jl_typeis(v,jl_typector_type)
#define jl_is_TypeConstructor(v)    jl_typeis(v,jl_typector_type)
#define jl_is_typename(v)    jl_typeis(v,jl_typename_type)
#define jl_is_int32(v)       jl_typeis(v,jl_int32_type)
#define jl_is_int64(v)       jl_typeis(v,jl_int64_type)
#define jl_is_uint32(v)      jl_typeis(v,jl_uint32_type)
#define jl_is_uint64(v)      jl_typeis(v,jl_uint64_type)
#define jl_is_float32(v)     jl_typeis(v,jl_float32_type)
#define jl_is_float64(v)     jl_typeis(v,jl_float64_type)
#define jl_is_bool(v)        jl_typeis(v,jl_bool_type)
#define jl_is_symbol(v)      jl_typeis(v,jl_sym_type)
#define jl_is_expr(v)        jl_typeis(v,jl_expr_type)
#define jl_is_symbolnode(v)  jl_typeis(v,jl_symbolnode_type)
#define jl_is_getfieldnode(v)  jl_typeis(v,jl_getfieldnode_type)
#define jl_is_labelnode(v)   jl_typeis(v,jl_labelnode_type)
#define jl_is_gotonode(v)    jl_typeis(v,jl_gotonode_type)
#define jl_is_quotenode(v)   jl_typeis(v,jl_quotenode_type)
#define jl_is_topnode(v)     jl_typeis(v,jl_topnode_type)
#define jl_is_linenode(v)    jl_typeis(v,jl_linenumbernode_type)
#define jl_is_lambda_info(v) jl_typeis(v,jl_lambda_info_type)
#define jl_is_module(v)      jl_typeis(v,jl_module_type)
#define jl_is_mtable(v)      jl_typeis(v,jl_methtable_type)
#define jl_is_task(v)        jl_typeis(v,jl_task_type)
#define jl_is_func(v)        (jl_typeis(v,jl_function_type) || jl_is_datatype(v))
#define jl_is_function(v)    jl_is_func(v)
#define jl_is_ascii_string(v) jl_typeis(v,jl_ascii_string_type)
#define jl_is_utf8_string(v) jl_typeis(v,jl_utf8_string_type)
#define jl_is_byte_string(v) (jl_is_ascii_string(v) || jl_is_utf8_string(v))
#define jl_is_cpointer(v)    jl_is_cpointer_type(jl_typeof(v))
#define jl_is_pointer(v)     jl_is_cpointer_type(jl_typeof(v))
#define jl_is_gf(f)          (((jl_function_t*)(f))->fptr==jl_apply_generic)

#define jl_tuple_len(t)   (((jl_tuple_t*)(t))->length)
#define jl_tuple_set_len_unsafe(t,n) (((jl_tuple_t*)(t))->length=(n))
#define jl_cell_data(a)   ((jl_value_t**)((jl_array_t*)a)->data)
#define jl_string_data(s) ((char*)((jl_array_t*)((jl_value_t**)(s))[1])->data)
#define jl_iostr_data(s)  ((char*)((jl_array_t*)((jl_value_t**)(s))[1])->data)

#define jl_gf_mtable(f) ((jl_methtable_t*)((jl_function_t*)(f))->env)
#define jl_gf_name(f)   (jl_gf_mtable(f)->name)

// get a pointer to the data in a datatype
#define jl_data_ptr(v)  (&((void**)(v))[1])

static inline int jl_is_bitstype(void *v)
{
    return (jl_is_datatype(v) && jl_isimmutable(v) &&
            jl_tuple_len(((jl_datatype_t*)(v))->names)==0 &&
            !((jl_datatype_t*)(v))->abstract &&
            ((jl_datatype_t*)(v))->size > 0);
}

static inline int jl_is_structtype(void *v)
{
    return (jl_is_datatype(v) &&
            (jl_tuple_len(((jl_datatype_t*)(v))->names) > 0 ||
             ((jl_datatype_t*)(v))->size == 0) &&
            !((jl_datatype_t*)(v))->abstract);
}

static inline int jl_isbits(void *t)   // corresponding to isbits() in julia
{
    return (jl_is_datatype(t) && !((jl_datatype_t*)t)->mutabl &&
            ((jl_datatype_t*)t)->pointerfree && !((jl_datatype_t*)t)->abstract);
}

static inline int jl_is_abstracttype(void *v)
{
    return (jl_is_datatype(v) && ((jl_datatype_t*)(v))->abstract);
}

static inline int jl_is_array_type(void *t)
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_array_typename);
}

static inline int jl_is_array(void *v)
{
    jl_value_t *t = jl_typeof(v);
    return jl_is_array_type(t);
}

static inline int jl_is_box(void *v)
{
    jl_value_t *t = jl_typeof(v);
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_box_typename);
}

static inline int jl_is_cpointer_type(void *t)
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_pointer_type->name);
}

static inline int jl_is_vararg_type(jl_value_t *v)
{
    return (jl_is_datatype(v) &&
            ((jl_datatype_t*)(v))->name == jl_vararg_type->name);
}

static inline int jl_is_ntuple_type(jl_value_t *v)
{
    return (jl_is_datatype(v) &&
            ((jl_datatype_t*)v)->name == jl_ntuple_typename);
}

static inline int jl_is_nontuple_type(jl_value_t *v)
{
    return (jl_typeis(v, jl_uniontype_type) ||
            jl_typeis(v, jl_datatype_type) ||
            jl_typeis(v, jl_typector_type));
}

static inline int jl_is_type_type(jl_value_t *v)
{
    return (jl_is_datatype(v) &&
            ((jl_datatype_t*)(v))->name == jl_type_type->name);
}

// type info accessors
jl_value_t *jl_full_type(jl_value_t *v);

// type predicates
int jl_is_type(jl_value_t *v);
DLLEXPORT int jl_is_leaf_type(jl_value_t *v);
int jl_has_typevars(jl_value_t *v);
int jl_tuple_subtype(jl_value_t **child, size_t cl,
                     jl_value_t **parent, size_t pl, int ta, int morespecific);
int jl_subtype(jl_value_t *a, jl_value_t *b, int ta);
int jl_type_morespecific(jl_value_t *a, jl_value_t *b, int ta);
int jl_subtype_invariant(jl_value_t *a, jl_value_t *b, int ta);
DLLEXPORT jl_value_t *jl_type_match(jl_value_t *a, jl_value_t *b);
jl_value_t *jl_type_match_morespecific(jl_value_t *a, jl_value_t *b);
DLLEXPORT int jl_types_equal(jl_value_t *a, jl_value_t *b);
int jl_types_equal_generic(jl_value_t *a, jl_value_t *b, int useenv);
jl_value_t *jl_type_union(jl_tuple_t *types);
jl_value_t *jl_type_intersection_matching(jl_value_t *a, jl_value_t *b,
                                          jl_tuple_t **penv, jl_tuple_t *tvars);
DLLEXPORT jl_value_t *jl_type_intersection(jl_value_t *a, jl_value_t *b);
int jl_args_morespecific(jl_value_t *a, jl_value_t *b);

// type constructors
jl_typename_t *jl_new_typename(jl_sym_t *name);
jl_tvar_t *jl_new_typevar(jl_sym_t *name,jl_value_t *lb,jl_value_t *ub);
jl_typector_t *jl_new_type_ctor(jl_tuple_t *params, jl_value_t *body);
jl_value_t *jl_apply_type(jl_value_t *tc, jl_tuple_t *params);
jl_value_t *jl_apply_type_(jl_value_t *tc, jl_value_t **params, size_t n);
jl_value_t *jl_instantiate_type_with(jl_value_t *t, jl_value_t **env, size_t n);
jl_uniontype_t *jl_new_uniontype(jl_tuple_t *types);
jl_datatype_t *jl_new_abstracttype(jl_value_t *name, jl_datatype_t *super,
                                   jl_tuple_t *parameters);
jl_datatype_t *jl_new_uninitialized_datatype(size_t nfields);
jl_datatype_t *jl_new_datatype(jl_sym_t *name, jl_datatype_t *super,
                               jl_tuple_t *parameters,
                               jl_tuple_t *fnames, jl_tuple_t *ftypes,
                               int abstract, int mutabl);
jl_datatype_t *jl_new_bitstype(jl_value_t *name, jl_datatype_t *super,
                               jl_tuple_t *parameters, size_t nbits);
jl_datatype_t *jl_wrap_Type(jl_value_t *t);  // x -> Type{x}
void jl_set_datatype_super(jl_datatype_t *tt, jl_value_t *super);

// constructors
jl_value_t *jl_new_bits(jl_datatype_t *bt, void *data);
void jl_assign_bits(void *dest, jl_value_t *bits);
DLLEXPORT jl_value_t *jl_new_struct(jl_datatype_t *type, ...);
DLLEXPORT jl_value_t *jl_new_structv(jl_datatype_t *type, jl_value_t **args, uint32_t na);
DLLEXPORT jl_value_t *jl_new_struct_uninit(jl_datatype_t *type);
jl_function_t *jl_new_closure(jl_fptr_t proc, jl_value_t *env,
                              jl_lambda_info_t *li);
jl_lambda_info_t *jl_new_lambda_info(jl_value_t *ast, jl_tuple_t *sparams);
jl_tuple_t *jl_tuple(size_t n, ...);
jl_tuple_t *jl_tuple1(void *a);
jl_tuple_t *jl_tuple2(void *a, void *b);
jl_tuple_t *jl_alloc_tuple(size_t n);
jl_tuple_t *jl_alloc_tuple_uninit(size_t n);
jl_tuple_t *jl_tuple_append(jl_tuple_t *a, jl_tuple_t *b);
jl_tuple_t *jl_tuple_fill(size_t n, jl_value_t *v);
DLLEXPORT jl_sym_t *jl_symbol(const char *str);
DLLEXPORT jl_sym_t *jl_symbol_lookup(const char *str);
DLLEXPORT jl_sym_t *jl_symbol_n(const char *str, int32_t len);
DLLEXPORT jl_sym_t *jl_gensym(void);
DLLEXPORT jl_sym_t *jl_tagged_gensym(const char *str, int32_t len);
jl_sym_t *jl_get_root_symbol(void);
jl_expr_t *jl_exprn(jl_sym_t *head, size_t n);
jl_function_t *jl_new_generic_function(jl_sym_t *name);
void jl_initialize_generic_function(jl_function_t *f, jl_sym_t *name);
void jl_add_method(jl_function_t *gf, jl_tuple_t *types, jl_function_t *meth,
                   jl_tuple_t *tvars);
jl_value_t *jl_method_def(jl_sym_t *name, jl_value_t **bp, jl_binding_t *bnd,
                          jl_tuple_t *argtypes, jl_function_t *f,
                          jl_tuple_t *tvars);
jl_value_t *jl_box_bool(int8_t x);
jl_value_t *jl_box_int8(int32_t x);
jl_value_t *jl_box_uint8(uint32_t x);
jl_value_t *jl_box_int16(int16_t x);
jl_value_t *jl_box_uint16(uint16_t x);
DLLEXPORT jl_value_t *jl_box_int32(int32_t x);
jl_value_t *jl_box_uint32(uint32_t x);
jl_value_t *jl_box_char(uint32_t x);
DLLEXPORT jl_value_t *jl_box_int64(int64_t x);
jl_value_t *jl_box_uint64(uint64_t x);
jl_value_t *jl_box_float32(float x);
jl_value_t *jl_box_float64(double x);
jl_value_t *jl_box_voidpointer(void *x);
jl_value_t *jl_box8 (jl_datatype_t *t, int8_t  x);
jl_value_t *jl_box16(jl_datatype_t *t, int16_t x);
jl_value_t *jl_box32(jl_datatype_t *t, int32_t x);
jl_value_t *jl_box64(jl_datatype_t *t, int64_t x);
DLLEXPORT int8_t jl_unbox_bool(jl_value_t *v);
DLLEXPORT int8_t jl_unbox_int8(jl_value_t *v);
DLLEXPORT uint8_t jl_unbox_uint8(jl_value_t *v);
DLLEXPORT int16_t jl_unbox_int16(jl_value_t *v);
DLLEXPORT uint16_t jl_unbox_uint16(jl_value_t *v);
DLLEXPORT int32_t jl_unbox_int32(jl_value_t *v);
DLLEXPORT uint32_t jl_unbox_uint32(jl_value_t *v);
DLLEXPORT int64_t jl_unbox_int64(jl_value_t *v);
DLLEXPORT uint64_t jl_unbox_uint64(jl_value_t *v);
DLLEXPORT float jl_unbox_float32(jl_value_t *v);
DLLEXPORT double jl_unbox_float64(jl_value_t *v);
DLLEXPORT void *jl_unbox_voidpointer(jl_value_t *v);

#ifdef _P64
#define jl_box_long(x)   jl_box_int64(x)
#define jl_box_ulong(x)   jl_box_uint64(x)
#define jl_unbox_long(x) jl_unbox_int64(x)
#define jl_is_long(x)    jl_is_int64(x)
#define jl_long_type     jl_int64_type
#else
#define jl_box_long(x)   jl_box_int32(x)
#define jl_box_ulong(x)   jl_box_uint32(x)
#define jl_unbox_long(x) jl_unbox_int32(x)
#define jl_is_long(x)    jl_is_int32(x)
#define jl_long_type     jl_int32_type
#endif

// structs
void jl_compute_field_offsets(jl_datatype_t *st);
int jl_field_index(jl_datatype_t *t, jl_sym_t *fld, int err);
DLLEXPORT jl_value_t *jl_get_nth_field(jl_value_t *v, size_t i);
jl_value_t *jl_set_nth_field(jl_value_t *v, size_t i, jl_value_t *rhs);
int jl_field_isdefined(jl_value_t *v, jl_sym_t *fld, int err);

// arrays
DLLEXPORT jl_array_t *jl_new_array(jl_value_t *atype, jl_tuple_t *dims);
DLLEXPORT jl_array_t *jl_new_arrayv(jl_value_t *atype, ...);
jl_array_t *jl_new_array_(jl_value_t *atype, uint32_t ndims, size_t *dims);
DLLEXPORT jl_array_t *jl_reshape_array(jl_value_t *atype, jl_array_t *data,
                                       jl_tuple_t *dims);
DLLEXPORT jl_array_t *jl_ptr_to_array_1d(jl_value_t *atype, void *data,
                                         size_t nel, int own_buffer);
DLLEXPORT jl_array_t *jl_ptr_to_array(jl_value_t *atype, void *data,
                                      jl_tuple_t *dims, int own_buffer);
int jl_array_store_unboxed(jl_value_t *el_type);

DLLEXPORT jl_array_t *jl_alloc_array_1d(jl_value_t *atype, size_t nr);
DLLEXPORT jl_array_t *jl_alloc_array_2d(jl_value_t *atype, size_t nr, size_t nc);
DLLEXPORT jl_array_t *jl_alloc_array_3d(jl_value_t *atype, size_t nr, size_t nc,
                                        size_t z);
DLLEXPORT jl_array_t *jl_pchar_to_array(const char *str, size_t len);
DLLEXPORT jl_value_t *jl_pchar_to_string(const char *str, size_t len);
DLLEXPORT jl_value_t *jl_cstr_to_string(const char *str);
DLLEXPORT jl_value_t *jl_array_to_string(jl_array_t *a);
DLLEXPORT jl_array_t *jl_alloc_cell_1d(size_t n);
DLLEXPORT jl_value_t *jl_arrayref(jl_array_t *a, size_t i);  // 0-indexed
DLLEXPORT void jl_arrayset(jl_array_t *a, jl_value_t *v, size_t i);  // 0-indexed
DLLEXPORT void jl_arrayunset(jl_array_t *a, size_t i);  // 0-indexed
int jl_array_isdefined(jl_value_t **args, int nargs);
DLLEXPORT void *jl_array_ptr(jl_array_t *a);
DLLEXPORT void jl_array_grow_end(jl_array_t *a, size_t inc);
DLLEXPORT void jl_array_del_end(jl_array_t *a, size_t dec);
DLLEXPORT void jl_array_grow_beg(jl_array_t *a, size_t inc);
DLLEXPORT void jl_array_del_beg(jl_array_t *a, size_t dec);
DLLEXPORT void jl_array_sizehint(jl_array_t *a, size_t sz);
DLLEXPORT void *jl_value_ptr(jl_value_t *a);
void jl_cell_1d_push(jl_array_t *a, jl_value_t *item);

// system information
DLLEXPORT int jl_errno(void);
DLLEXPORT jl_value_t *jl_strerror(int errnum);
DLLEXPORT int32_t jl_stat(const char* path, char* statbuf);

// environment entries
DLLEXPORT jl_value_t *jl_environ(int i);
#ifdef __WIN32__
DLLEXPORT jl_value_t *jl_env_done(char *pos);
#endif

DLLEXPORT int jl_spawn(char *name, char **argv, uv_loop_t *loop,
                       uv_process_t *proc, jl_value_t *julia_struct,
                       uv_handle_type stdin_type,uv_pipe_t *stdin_pipe,
                       uv_handle_type stdout_type,uv_pipe_t *stdout_pipe,
                       uv_handle_type stderr_type,uv_pipe_t *stderr_pipe);
DLLEXPORT void jl_run_event_loop(uv_loop_t *loop);
DLLEXPORT int jl_run_once(uv_loop_t *loop);
DLLEXPORT int jl_process_events(uv_loop_t *loop);

DLLEXPORT uv_loop_t *jl_global_event_loop();

DLLEXPORT uv_pipe_t *jl_make_pipe(int writable, int julia_only, jl_value_t *julia_struct);
DLLEXPORT void jl_close_uv(uv_handle_t *handle);

DLLEXPORT int16_t jl_start_reading(uv_stream_t *handle);

DLLEXPORT void jl_callback(void *callback);

DLLEXPORT uv_async_t *jl_make_async(uv_loop_t *loop, jl_value_t *julia_struct);
DLLEXPORT void jl_async_send(uv_async_t *handle);
DLLEXPORT uv_idle_t * jl_make_idle(uv_loop_t *loop, jl_value_t *julia_struct);
DLLEXPORT int jl_idle_start(uv_idle_t *idle);
DLLEXPORT int jl_idle_stop(uv_idle_t *idle);

DLLEXPORT int jl_putc(unsigned char c, uv_stream_t *stream);
DLLEXPORT int jl_puts(char *str, uv_stream_t *stream);
DLLEXPORT int jl_pututf8(uv_stream_t *s, uint32_t wchar);

DLLEXPORT uv_timer_t *jl_make_timer(uv_loop_t *loop, jl_value_t *julia_struct);
DLLEXPORT int jl_timer_stop(uv_timer_t* timer);

DLLEXPORT uv_tcp_t *jl_tcp_init(uv_loop_t *loop);
DLLEXPORT int jl_tcp_bind(uv_tcp_t* handle, uint16_t port, uint32_t host);

DLLEXPORT void NORETURN jl_exit(int status);

DLLEXPORT size_t jl_sizeof_uv_stream_t();
DLLEXPORT size_t jl_sizeof_uv_pipe_t();
DLLEXPORT int jl_sizeof_ios_t();

#ifdef __WIN32__
DLLEXPORT struct tm* localtime_r(const time_t *t, struct tm *tm);
#endif

// exceptions
void NORETURN jl_error(const char *str);
void NORETURN jl_errorf(const char *fmt, ...);
void jl_too_few_args(const char *fname, int min);
void jl_too_many_args(const char *fname, int max);
void jl_type_error(const char *fname, jl_value_t *expected, jl_value_t *got);
void jl_type_error_rt(const char *fname, const char *context,
                      jl_value_t *ty, jl_value_t *got);
jl_value_t *jl_no_method_error(jl_function_t *f, jl_value_t **args, size_t na);
void jl_check_type_tuple(jl_tuple_t *t, jl_sym_t *name, const char *ctx);

// initialization functions
DLLEXPORT void julia_init(char *imageFile);
DLLEXPORT
int julia_trampoline(int argc, char *argv[], int (*pmain)(int ac,char *av[]));
void jl_init_types(void);
void jl_init_box_caches(void);
void jl_init_frontend(void);
void jl_init_primitives(void);
void jl_init_codegen(void);
void jl_init_intrinsic_functions(void);
void jl_init_tasks(void *stack, size_t ssize);
void jl_init_serializer(void);

void jl_save_system_image(char *fname);
void jl_restore_system_image(char *fname);

// front end interface
DLLEXPORT jl_value_t *jl_parse_input_line(const char *str);
DLLEXPORT jl_value_t *jl_parse_string(const char *str, int pos0, int greedy);
void jl_start_parsing_file(const char *fname);
void jl_stop_parsing();
jl_value_t *jl_parse_next();
DLLEXPORT void jl_load_file_string(const char *text, char *filename);
DLLEXPORT jl_value_t *jl_expand(jl_value_t *expr);
jl_lambda_info_t *jl_wrap_expr(jl_value_t *expr);

// some useful functions
DLLEXPORT void jl_show(jl_value_t *stream, jl_value_t *v);
void jl_show_tuple(jl_value_t *st, jl_tuple_t *t, char opn, char cls, int comma_one);
DLLEXPORT jl_value_t *jl_stdout_obj();
DLLEXPORT jl_value_t *jl_stderr_obj();
DLLEXPORT int jl_egal(jl_value_t *a, jl_value_t *b);
DLLEXPORT uptrint_t jl_object_id(jl_value_t *v);

// modules
extern DLLEXPORT jl_module_t *jl_main_module;
extern DLLEXPORT jl_module_t *jl_core_module;
extern DLLEXPORT jl_module_t *jl_base_module;
extern DLLEXPORT jl_module_t *jl_current_module;
jl_module_t *jl_new_module(jl_sym_t *name);
// get binding for reading
DLLEXPORT jl_binding_t *jl_get_binding(jl_module_t *m, jl_sym_t *var);
// get binding for assignment
jl_binding_t *jl_get_binding_wr(jl_module_t *m, jl_sym_t *var);
jl_binding_t *jl_get_binding_for_method_def(jl_module_t *m, jl_sym_t *var);
DLLEXPORT int jl_boundp(jl_module_t *m, jl_sym_t *var);
DLLEXPORT int jl_defines_or_exports_p(jl_module_t *m, jl_sym_t *var);
DLLEXPORT int jl_is_const(jl_module_t *m, jl_sym_t *var);
DLLEXPORT jl_value_t *jl_get_global(jl_module_t *m, jl_sym_t *var);
DLLEXPORT void jl_set_global(jl_module_t *m, jl_sym_t *var, jl_value_t *val);
DLLEXPORT void jl_set_const(jl_module_t *m, jl_sym_t *var, jl_value_t *val);
void jl_checked_assignment(jl_binding_t *b, jl_value_t *rhs);
void jl_declare_constant(jl_binding_t *b);
void jl_module_using(jl_module_t *to, jl_module_t *from);
void jl_module_use(jl_module_t *to, jl_module_t *from, jl_sym_t *s);
void jl_module_import(jl_module_t *to, jl_module_t *from, jl_sym_t *s);
void jl_module_importall(jl_module_t *to, jl_module_t *from);
DLLEXPORT void jl_module_export(jl_module_t *from, jl_sym_t *s);
void jl_add_standard_imports(jl_module_t *m);

// external libraries
enum JL_RTLD_CONSTANT {
     JL_RTLD_LOCAL=0U, JL_RTLD_GLOBAL=1U, /* LOCAL=0 since it is the default */
     JL_RTLD_LAZY=2U, JL_RTLD_NOW=4U,
     /* Linux/glibc and MacOS X: */
     JL_RTLD_NODELETE=8U, JL_RTLD_NOLOAD=16U, 
     /* Linux/glibc: */ JL_RTLD_DEEPBIND=32U,
     /* MacOS X 10.5+: */ JL_RTLD_FIRST=64U
};
#define JL_RTLD_DEFAULT (JL_RTLD_LAZY | JL_RTLD_DEEPBIND)
DLLEXPORT uv_lib_t *jl_load_dynamic_library(char *fname, unsigned flags);
DLLEXPORT uv_lib_t *jl_load_dynamic_library_e(char *fname, unsigned flags);
DLLEXPORT void *jl_dlsym_e(uv_lib_t *handle, char *symbol);
DLLEXPORT void *jl_dlsym(uv_lib_t *handle, char *symbol);
DLLEXPORT uv_lib_t *jl_wrap_raw_dl_handle(void *handle);
void *jl_dlsym_e(uv_lib_t *handle, char *symbol); //supress errors
void *jl_dlsym_win32(char *name);
DLLEXPORT int add_library_mapping(char *lib, void *hnd);

// event loop
DLLEXPORT void jl_runEventLoop();
DLLEXPORT void jl_processEvents();

// compiler
void jl_compile(jl_function_t *f);
void jl_generate_fptr(jl_function_t *f);
DLLEXPORT jl_value_t *jl_toplevel_eval(jl_value_t *v);
jl_value_t *jl_eval_global_var(jl_module_t *m, jl_sym_t *e);
DLLEXPORT void jl_load(const char *fname);
void jl_parse_eval_all(char *fname);
jl_value_t *jl_interpret_toplevel_thunk(jl_lambda_info_t *lam);
jl_value_t *jl_interpret_toplevel_expr(jl_value_t *e);
jl_value_t *jl_interpret_toplevel_expr_with(jl_value_t *e,
                                            jl_value_t **locals, size_t nl);
jl_value_t *jl_interpret_toplevel_expr_in(jl_module_t *m, jl_value_t *e,
                                          jl_value_t **locals, size_t nl);
jl_module_t *jl_base_relative_to(jl_module_t *m);
void jl_type_infer(jl_lambda_info_t *li, jl_tuple_t *argtypes,
                   jl_lambda_info_t *def);

DLLEXPORT void jl_show_method_table(jl_value_t *outstr, jl_function_t *gf);
jl_lambda_info_t *jl_add_static_parameters(jl_lambda_info_t *l, jl_tuple_t *sp);
jl_function_t *jl_method_lookup_by_type(jl_methtable_t *mt, jl_tuple_t *types,
                                        int cache);
jl_function_t *jl_method_lookup(jl_methtable_t *mt, jl_value_t **args, size_t nargs, int cache);
jl_value_t *jl_gf_invoke(jl_function_t *gf, jl_tuple_t *types,
                         jl_value_t **args, size_t nargs);

// AST access
jl_array_t *jl_lam_args(jl_expr_t *l);
jl_array_t *jl_lam_locals(jl_expr_t *l);
jl_array_t *jl_lam_vinfo(jl_expr_t *l);
jl_array_t *jl_lam_capt(jl_expr_t *l);
jl_expr_t *jl_lam_body(jl_expr_t *l);
jl_value_t *jl_ast_rettype(jl_lambda_info_t *li, jl_value_t *ast);
jl_sym_t *jl_decl_var(jl_value_t *ex);
DLLEXPORT int jl_is_rest_arg(jl_value_t *ex);

jl_value_t *jl_prepare_ast(jl_lambda_info_t *li, jl_tuple_t *sparams);

jl_value_t *jl_compress_ast(jl_lambda_info_t *li, jl_value_t *ast);
jl_value_t *jl_uncompress_ast(jl_lambda_info_t *li, jl_value_t *data);

static inline int jl_vinfo_capt(jl_array_t *vi)
{
    return (jl_unbox_long(jl_cellref(vi,2))&1)!=0;
}

static inline int jl_vinfo_assigned(jl_array_t *vi)
{
    return (jl_unbox_long(jl_cellref(vi,2))&2)!=0;
}

static inline int jl_vinfo_assigned_inner(jl_array_t *vi)
{
    return (jl_unbox_long(jl_cellref(vi,2))&4)!=0;
}

// for writing julia functions in C
#define JL_CALLABLE(name) \
    jl_value_t *name(jl_value_t *F, jl_value_t **args, uint32_t nargs)

static inline
jl_value_t *jl_apply(jl_function_t *f, jl_value_t **args, uint32_t nargs)
{
    return f->fptr((jl_value_t*)f, args, nargs);
}

#define JL_NARGS(fname, min, max)                               \
    if (nargs < min) jl_too_few_args(#fname, min);              \
    else if (nargs > max) jl_too_many_args(#fname, max);

#define JL_NARGSV(fname, min)                           \
    if (nargs < min) jl_too_few_args(#fname, min);

#define JL_TYPECHK(fname, type, v)                                      \
    if (!jl_is_##type(v)) {                                             \
        jl_type_error(#fname, (jl_value_t*)jl_##type##_type, (v));      \
    }

// gc

#ifdef JL_GC_MARKSWEEP
typedef struct _jl_gcframe_t {
    size_t nroots;
    struct _jl_gcframe_t *prev;
    // actual roots go here
} jl_gcframe_t;

// NOTE: it is the caller's responsibility to make sure arguments are
// rooted. foo(f(), g()) will not work, and foo can't do anything about it,
// so the caller must do
// jl_value_t *x=NULL, *y=NULL; JL_GC_PUSH(&x, &y);
// x = f(); y = g(); foo(x, y)

extern DLLEXPORT jl_gcframe_t *jl_pgcstack;

#define JL_GC_PUSH(...)                                                   \
  void *__gc_stkf[] = {(void*)((VA_NARG(__VA_ARGS__)<<1)|1), jl_pgcstack, \
                       __VA_ARGS__};                                      \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSHARGS(rts_var,n)                               \
  rts_var = ((jl_value_t**)alloca(((n)+2)*sizeof(jl_value_t*)))+2;    \
  ((void**)rts_var)[-2] = (void*)(((size_t)n)<<1);              \
  ((void**)rts_var)[-1] = jl_pgcstack;                          \
  jl_pgcstack = (jl_gcframe_t*)&(((void**)rts_var)[-2])

#define JL_GC_POP() (jl_pgcstack = jl_pgcstack->prev)

void jl_gc_init(void);
void jl_gc_setmark(jl_value_t *v);
DLLEXPORT void jl_gc_enable(void);
DLLEXPORT void jl_gc_disable(void);
DLLEXPORT int jl_gc_is_enabled(void);
void jl_gc_ephemeral_on(void);
void jl_gc_ephemeral_off(void);
DLLEXPORT void jl_gc_collect(void);
void jl_gc_preserve(jl_value_t *v);
void jl_gc_unpreserve(void);
int jl_gc_n_preserved_values(void);
DLLEXPORT void jl_gc_add_finalizer(jl_value_t *v, jl_function_t *f);
jl_weakref_t *jl_gc_new_weakref(jl_value_t *value);
jl_mallocptr_t *jl_gc_acquire_buffer(void *b, size_t sz);
jl_mallocptr_t *jl_gc_managed_malloc(size_t sz);
void *alloc_2w(void);
void *alloc_3w(void);
void *alloc_4w(void);

#else

#define JL_GC_PUSH(...) ;
#define JL_GC_PUSHARGS(rts,n) ;
#define JL_GC_POP()

#define jl_gc_preserve(v) ((void)(v))
#define jl_gc_unpreserve()
#define jl_gc_n_preserved_values() (0)

static inline void *alloc_2w() { return allocobj(2*sizeof(void*)); }
static inline void *alloc_3w() { return allocobj(3*sizeof(void*)); }
static inline void *alloc_4w() { return allocobj(4*sizeof(void*)); }
#endif

// asynch signal handling

#include <signal.h>

DLLEXPORT extern volatile sig_atomic_t jl_signal_pending;
DLLEXPORT extern volatile sig_atomic_t jl_defer_signal;
DLLEXPORT void jl_handle_sigint();

#define JL_SIGATOMIC_BEGIN() (jl_defer_signal++)
#define JL_SIGATOMIC_END()                                      \
    do {                                                        \
        jl_defer_signal--;                                      \
        if (jl_defer_signal == 0 && jl_signal_pending != 0)     \
            raise(jl_signal_pending);                           \
    } while(0)

DLLEXPORT void restore_signals(void);

// tasks and exceptions

// info describing an exception handler
typedef struct _jl_handler_t {
    jl_jmp_buf eh_ctx;
#ifdef JL_GC_MARKSWEEP
    jl_gcframe_t *gcstack;
#endif
    struct _jl_handler_t *prev;
} jl_handler_t;

typedef struct _jl_task_t {
    JL_DATA_TYPE
    struct _jl_task_t *on_exit;
    struct _jl_task_t *last;
    jl_value_t *tls;
    jl_value_t *consumers;
    int8_t done;
    int8_t runnable;
    jl_value_t *result;
    jl_jmp_buf ctx;
    union {
        void *stackbase;
        void *stack;
    };
    jl_jmp_buf base_ctx;
    size_t bufsz;
    void *stkbuf;
    size_t ssize;
    jl_function_t *start;
    // current exception handler
    jl_handler_t *eh;
    // saved gc stack top for context switches
    jl_gcframe_t *gcstack;
} jl_task_t;

typedef union jl_any_stream {
    ios_t ios;
    uv_stream_t stream;
} jl_any_stream;

DLLEXPORT void jl_uv_associate_julia_struct(uv_handle_t *handle, jl_value_t *data);
DLLEXPORT int jl_uv_fs_result(uv_fs_t *f);

extern DLLEXPORT jl_task_t * volatile jl_current_task;
extern DLLEXPORT jl_task_t *jl_root_task;
extern DLLEXPORT jl_value_t *jl_exception_in_transit;

jl_task_t *jl_new_task(jl_function_t *start, size_t ssize);
jl_value_t *jl_switchto(jl_task_t *t, jl_value_t *arg);
DLLEXPORT void NORETURN jl_throw(jl_value_t *e);
DLLEXPORT void NORETURN jl_throw_with_superfluous_argument(jl_value_t *e, int);
DLLEXPORT void NORETURN jl_rethrow();
DLLEXPORT void NORETURN jl_rethrow_other(jl_value_t *e);

DLLEXPORT jl_array_t *jl_takebuf_array(ios_t *s);
DLLEXPORT jl_value_t *jl_takebuf_string(ios_t *s);
DLLEXPORT void *jl_takebuf_raw(ios_t *s);
DLLEXPORT jl_value_t *jl_readuntil(ios_t *s, uint8_t delim);
DLLEXPORT void jl_free2(void *p, void *hint);

DLLEXPORT int jl_cpu_cores(void);

DLLEXPORT size_t jl_write(uv_stream_t *stream, const char *str, size_t n);
DLLEXPORT int jl_printf(uv_stream_t *s, const char *format, ...);
DLLEXPORT int jl_vprintf(uv_stream_t *s, const char *format, va_list args);

DLLEXPORT size_t rec_backtrace(ptrint_t *data, size_t maxsize);

#define JL_STREAM uv_stream_t
#define JL_STDOUT jl_uv_stdout
#define JL_STDERR jl_uv_stderr
#define JL_STDIN  jl_uv_stdin
#define JL_PRINTF jl_printf
#define JL_PUTC	  jl_putc
#define JL_PUTS	  jl_puts
#define JL_WRITE  jl_write

//IO objects
extern DLLEXPORT uv_stream_t *jl_uv_stdin; //these are actually uv_tty_t's and can be cast to such, but that gives warnings whenver they are used as streams
extern DLLEXPORT uv_stream_t * jl_uv_stdout;
extern DLLEXPORT uv_stream_t * jl_uv_stderr;

DLLEXPORT JL_STREAM *jl_stdout_stream();
DLLEXPORT JL_STREAM *jl_stdin_stream();
DLLEXPORT JL_STREAM *jl_stderr_stream();

static inline void jl_eh_restore_state(jl_handler_t *eh)
{
    JL_SIGATOMIC_BEGIN();
    jl_current_task->eh = eh->prev;
#ifdef JL_GC_MARKSWEEP
    jl_pgcstack = eh->gcstack;
#endif
    JL_SIGATOMIC_END();
}

DLLEXPORT void jl_enter_handler(jl_handler_t *eh);
DLLEXPORT void jl_pop_handler(int n);

#if defined(__WIN32__)
int __attribute__ ((__nothrow__,__returns_twice__)) jl_setjmp(jmp_buf _Buf);
__declspec(noreturn) __attribute__ ((__nothrow__)) void jl_longjmp(jmp_buf _Buf,int _Value);
#define jl_setjmp_f    jl_setjmp
#define jl_setjmp(a,b) jl_setjmp(a)
#define jl_longjmp(a,b) jl_longjmp(a,b)
#else
// determine actual entry point name
#if defined(sigsetjmp)
#define jl_setjmp_f    __sigsetjmp
#else
#define jl_setjmp_f    sigsetjmp
#endif
#define jl_setjmp(a,b) sigsetjmp(a,b)
#define jl_longjmp(a,b) siglongjmp(a,b)
#endif

#define JL_TRY                                                    \
    int i__tr, i__ca; jl_handler_t __eh;                          \
    jl_enter_handler(&__eh);                                      \
    if (!jl_setjmp(__eh.eh_ctx,0))                                \
        for (i__tr=1; i__tr; i__tr=0, jl_eh_restore_state(&__eh))

#define JL_EH_POP() jl_eh_restore_state(&__eh)

#ifdef __WIN32__
#define JL_CATCH                                                \
    else                                                        \
        for (i__ca=1, jl_eh_restore_state(&__eh); i__ca; i__ca=0) \
            if (((jl_exception_in_transit==jl_stackovf_exception) && _resetstkoflw()) || 1)
#else
#define JL_CATCH                                                \
    else                                                        \
        for (i__ca=1, jl_eh_restore_state(&__eh); i__ca; i__ca=0)
#endif

#endif
