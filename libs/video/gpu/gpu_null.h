/*
 * ps3recomp - the portable renderer's GPU layer: the null backend's own calls
 *
 * The null backend (gpu_null_create) records and checks and draws nothing,
 * for headless CI on every system. These are the calls CI makes beside
 * gpu.h's: the stream hash, its text dump, the check counts and the test
 * hooks. Plain C99, like gpu.h.
 *
 * What it does:
 *   - buffers are CPU memory: maps work, copies between buffers copy bytes,
 *     an imported buffer is the caller's memory. Textures hold nothing: a
 *     copy from a texture into a buffer writes zeroes, and occlusion results
 *     are zero. Draws, clears and resolves change nothing;
 *   - the "GPU" is instant: work is done when it is submitted, so fences
 *     complete at submit and waits never block. lost() stays 0 unless
 *     gpu_null_test_lose says otherwise;
 *   - every command and the device calls around them are checked against
 *     the contract (gpu.h, gpu_cmd_ops.h) as D3D12 and Vulkan both need it.
 *     Each failure is counted by kind (GPU_NULL_ERR_*), and the first of each
 *     kind is printed to stderr with the command and its arguments.
 *
 * The operation stream, and its hash. Every recorded command, in submission
 * order, plus the render thread's device calls that change what the GPU
 * would see (the creations, frees, effective table writes, acquires,
 * presents, submits and signals), in the order they were made. A device call
 * goes into the stream before the next submit's commands; a submit is its
 * record, then for each recorder a "cmd <slot>" record and its commands.
 * Each record is
 *     its name (the command's or call's name, as gpu_cmd_ops.h and gpu.h
 *     spell it) and one zero byte; its argument count as a 32-bit
 *     little-endian number; then each argument as one tag byte
 *     (GPU_NULL_T_*) and a 64-bit little-endian value. Text arguments are
 *     left out of both.
 * The hash is FNV-1a 64 over those bytes. Handles are their creation number
 * within their kind (1 the first texture made, ...; 0 none), so a run hashes
 * the same on every machine. Pipelines, which pipeline workers make in any
 * order, are named by their contents instead: the description's bytes, its
 * shaders' code and its layout's slots (as the D3D12 backend names them for
 * its library). An address is its buffer's number times 2^40 plus the
 * offset; a table its pool's number times 2^32 plus the entry; a float its
 * bits. Pointer arguments are replaced by what they point at (a pointer
 * that may be NULL by a flag and zeroes). No pointer, timing or thread
 * order is in it. gpu_cmd_marker's text is not hashed (it is a crash
 * tool's, and may change without the GPU's work changing); the dump shows
 * it.
 */
#ifndef PS3RECOMP_GPU_NULL_H
#define PS3RECOMP_GPU_NULL_H

#if defined(PS3RECOMP_RSX_HAL)

#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The kinds of check (gpu_null_errors). */
enum {
    GPU_NULL_ERR_HANDLE = 0,   /* a stale, freed or wrong-kind handle (its generation) */
    GPU_NULL_ERR_FREED,        /* freed before the fence of the work using it, or while a recording uses it */
    GPU_NULL_ERR_BARRIER,      /* a barrier's before state is not the subresource's tracked state */
    GPU_NULL_ERR_STATE,        /* a texture or buffer used in a state that does not allow it */
    GPU_NULL_ERR_DRAW,         /* a draw without a layout, pipeline or pass, or with a slot unbound */
    GPU_NULL_ERR_USE,          /* used without its GPU_USE bit, or an address outside its buffer */
    GPU_NULL_ERR_ALIGN,        /* a constant buffer address off cb_align, or another alignment */
    GPU_NULL_ERR_BOUNDS,       /* a copy's box or footprint outside its texture or buffer */
    GPU_NULL_ERR_QUERY,        /* a query begun twice, ended unbegun, not reset, left open */
    GPU_NULL_ERR_TABLE,        /* a table entry written while unfinished work uses it, or out of its pool */
    GPU_NULL_ERR_FENCE,        /* a fence the timeline never reached, or a slot or readback before its fence */
    GPU_NULL_ERR_RECORDER,     /* a recorder used after its end, ended or submitted twice */
    GPU_NULL_ERR_SWAP,         /* a swap chain image used without acquire, or presented unready */
    GPU_NULL_ERR_COUNT
};
#define GPU_NULL_ERR_ALL (-1)

/* The argument tags of the stream (gpu_null_arg.tag). */
enum {
    GPU_NULL_T_U     = 'u',   /* a number */
    GPU_NULL_T_I     = 'i',   /* a signed number (two's complement in the value) */
    GPU_NULL_T_X     = 'x',   /* bits: states, flags, push constants */
    GPU_NULL_T_F     = 'f',   /* a float's bits */
    GPU_NULL_T_FLAG  = '?',   /* 1: the optional argument after it is there; 0: NULL, zeroes follow */
    GPU_NULL_T_BUF   = 'B',   /* handles: their creation number */
    GPU_NULL_T_TEX   = 'X',
    GPU_NULL_T_VIEW  = 'V',
    GPU_NULL_T_SAMP  = 'S',
    GPU_NULL_T_LAYOUT = 'L',
    GPU_NULL_T_QPOOL = 'Q',
    GPU_NULL_T_TPOOL = 'O',
    GPU_NULL_T_HEAP  = 'H',
    GPU_NULL_T_SWAP  = 'W',
    GPU_NULL_T_SHADER = 'Z',
    GPU_NULL_T_PIPE  = 'P',   /* a pipeline: its contents' name */
    GPU_NULL_T_ADDR  = 'A',   /* buffer number << 40 | offset */
    GPU_NULL_T_TABLE = 'T',   /* pool number << 32 | first entry */
    GPU_NULL_T_TEXT  = 's',   /* not hashed: a text's length, its bytes in the GPU_NULL_T_CHARS after it */
    GPU_NULL_T_CHARS = 'c'
};

/* The stream's FNV-1a 64 so far, everything made and submitted included. */
uint64_t gpu_null_stream_hash(gpu_device* d);

/* Writes the stream as text, one record a line, from now on (the
 * GPU_NULL_DUMP=<file> environment variable does it from the start). NULL
 * stops it. 1 when the file opened. */
int gpu_null_dump(gpu_device* d, const char* path);

/* Failed checks of one kind since the start or the last reset, or of all
 * kinds (GPU_NULL_ERR_ALL). */
uint32_t    gpu_null_errors(gpu_device* d, int kind);
/* Counts back to 0; the next failure of each kind is printed again. */
void        gpu_null_errors_reset(gpu_device* d);
const char* gpu_null_error_name(int kind);

/* What the stream holds so far; and, outside it, the pipelines the device
 * keeps (one per contents) and the GPU_PIPELINE_STORE_ONLY creates, which
 * keep none. */
typedef struct gpu_null_counts {
    uint64_t submits, commands, draws, table_writes, presents;
    uint64_t pipelines, stored_only;
} gpu_null_counts;
void gpu_null_counts_get(gpu_device* d, gpu_null_counts* out);

/* Test hook: the device is lost with this reason (nonzero), as a removed
 * D3D12 device is: lost() says so, the fence reads UINT64_MAX and waits end
 * at once. */
void gpu_null_test_lose(gpu_device* d, uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HAL */
#endif /* PS3RECOMP_GPU_NULL_H */
