//===-- dfsan.cc ----------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of DataFlowSanitizer.
//
// DataFlowSanitizer runtime.  This file defines the public interface to
// DataFlowSanitizer as well as the definition of certain runtime functions
// called automatically by the compiler (specifically the instrumentation pass
// in llvm/lib/Transforms/Instrumentation/DataFlowSanitizer.cpp).
//
// The public interface is defined in include/sanitizer/dfsan_interface.h whose
// functions are prefixed dfsan_ while the compiler interface functions are
// prefixed __dfsan_.
//===----------------------------------------------------------------------===//

#include "../sanitizer_common/sanitizer_atomic.h"
#include "../sanitizer_common/sanitizer_common.h"
#include "../sanitizer_common/sanitizer_file.h"
#include "../sanitizer_common/sanitizer_flags.h"
#include "../sanitizer_common/sanitizer_flag_parser.h"
#include "../sanitizer_common/sanitizer_libc.h"
#include "../sanitizer_common/sanitizer_mutex.h"
#include "../sanitizer_common/sanitizer_posix.h"

#include "dfsan.h"
#include "taint_allocator.h"
#include "union_util.h"
#include "union_hashtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <fcntl.h>

#include <z3++.h>

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define OPTIMISTIC 1

using namespace __dfsan;

typedef atomic_uint32_t atomic_dfsan_label;
static const dfsan_label kInitializingLabel = -1;

static atomic_dfsan_label __dfsan_last_label;
static dfsan_label_info *__dfsan_label_info;

// taint source
static struct taint_file tainted;

// Hash table
static const uptr hashtable_size = (1ULL << 32);
static const size_t union_table_size = (1ULL << 18);
static __taint::union_hashtable __union_table(union_table_size);

// for output
static const char* __output_dir;
static u32 __instance_id;
static u32 __session_id;
static u32 __current_index = 0;
static z3::context __z3_context;
static z3::solver __z3_solver(__z3_context, "QF_BV");

// filter?
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 __taint_trace_callstack;
typedef std::pair<u32, void*> trace_context;
struct context_hash {
  std::size_t operator()(const trace_context &context) const {
    return std::hash<u32>{}(context.first) ^ std::hash<void*>{}(context.second);
  }
};
static std::unordered_map<trace_context, u16, context_hash> __branches;
static const u16 MAX_BRANCH_COUNT = 16;
static const u64 MAX_GEP_INDEX = 0x10000;

// dependencies
struct expr_hash {
  std::size_t operator()(const z3::expr &expr) const {
    return expr.hash();
  }
};
struct expr_equal {
  bool operator()(const z3::expr &lhs, const z3::expr &rhs) const {
    return lhs.id() == rhs.id();
  }
};
typedef std::unordered_set<z3::expr, expr_hash, expr_equal> expr_set_t;
typedef struct {
  expr_set_t expr_deps;
  std::unordered_set<dfsan_label> input_deps;
} branch_dep_t;
static std::vector<branch_dep_t*> *__branch_deps;

Flags __dfsan::flags_data;
bool print_debug;

SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL dfsan_label __dfsan_retval_tls;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL dfsan_label __dfsan_arg_tls[64];

SANITIZER_INTERFACE_ATTRIBUTE uptr __dfsan_shadow_ptr_mask;

// On Linux/x86_64, memory is laid out as follows:
//
// +--------------------+ 0x800000000000 (top of memory)
// | application memory |
// +--------------------+ 0x700000040000 (kAppAddr)
// |--------------------| UnusedAddr()
// |                    |
// |    hash table      |
// |                    |
// +--------------------+ 0x4000c0000000 (kHashTableAddr)
// |    union table     |
// +--------------------+ 0x400000000000 (kUnionTableAddr)
// |   shadow memory    |
// +--------------------+ 0x000000100000 (kShadowAddr)
// |       unused       |
// +--------------------+ 0x000000010000 (kKernelAddr)
// | reserved by kernel |
// +--------------------+ 0x000000000000
//
// To derive a shadow memory address from an application memory address,
// bits 44-46 are cleared to bring the address into the range
// [0x000000040000,0x100000000000).  Then the address is shifted left by 2 to
// account for the double byte representation of shadow labels and move the
// address into the shadow memory range.  See the function shadow_for below.

#ifdef DFSAN_RUNTIME_VMA
// Runtime detected VMA size.
int __dfsan::vmaSize;
#endif

static uptr UnusedAddr() {
  return MappingArchImpl<MAPPING_HASH_TABLE_ADDR>() + hashtable_size;
}

// Checks we do not run out of labels.
static void dfsan_check_label(dfsan_label label) {
  if (label == kInitializingLabel) {
    Report("FATAL: Taint: out of labels\n");
    Die();
  } else if ((uptr)(&__dfsan_label_info[label]) >= HashTableAddr()) {
    Report("FATAL: Exhausted labels\n");
    Die();
  }
}

// based on https://github.com/Cyan4973/xxHash
// simplified since we only have 12 bytes info
static inline u32 xxhash(u32 h1, u32 h2, u32 h3) {
  const u32 PRIME32_1 = 2654435761U;
  const u32 PRIME32_2 = 2246822519U;
  const u32 PRIME32_3 = 3266489917U;
  const u32 PRIME32_4 =  668265263U;
  const u32 PRIME32_5 =  374761393U;

  #define XXH_rotl32(x,r) ((x << r) | (x >> (32 - r)))
  u32 h32 = PRIME32_5;
  h32 += h1 * PRIME32_3;
  h32  = XXH_rotl32(h32, 17) * PRIME32_4;
  h32 += h2 * PRIME32_3;
  h32  = XXH_rotl32(h32, 17) * PRIME32_4;
  h32 += h3 * PRIME32_3;
  h32  = XXH_rotl32(h32, 17) * PRIME32_4;
  #undef XXH_rotl32

  h32 ^= h32 >> 15;
  h32 *= PRIME32_2;
  h32 ^= h32 >> 13;
  h32 *= PRIME32_3;
  h32 ^= h32 >> 16;

  return h32;
}

static inline dfsan_label_info* get_label_info(dfsan_label label) {
  return &__dfsan_label_info[label];
}

static inline bool is_constant_label(dfsan_label label) {
  return label == CONST_LABEL;
}

static inline bool is_kind_of_label(dfsan_label label, u16 kind) {
  return get_label_info(label)->op == kind;
}

static bool isZeroOrPowerOfTwo(uint16_t x) { return (x & (x - 1)) == 0; }

static inline branch_dep_t* get_branch_dep(size_t n) {
  if (tainted.is_stdin && n >= __branch_deps->size()) {
    __branch_deps->resize(n + 1);
  }
  return __branch_deps->at(n);
}

static inline void set_branch_dep(size_t n, branch_dep_t* dep) {
  if (tainted.is_stdin && n >= __branch_deps->size()) {
    __branch_deps->resize(n + 1);
  }
  __branch_deps->at(n) = dep;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
dfsan_label __taint_union(dfsan_label l1, dfsan_label l2, u16 op, u16 size,
                          u64 op1, u64 op2) {
  if (l1 > l2 && is_commutative(op)) {
    // needs to swap both labels and concretes
    Swap(l1, l2);
    Swap(op1, op2);
  }
  if (l1 == 0 && l2 < CONST_OFFSET && op != fsize) return 0;
  if (l1 == kInitializingLabel || l2 == kInitializingLabel) return kInitializingLabel;

  if (l1 >= CONST_OFFSET) op1 = 0;
  if (l2 >= CONST_OFFSET) op2 = 0;

  struct dfsan_label_info label_info = {
    .l1 = l1, .l2 = l2, .op1 = op1, .op2 = op2, .op = op, .size = size,
    .flags = 0, .tree_size = 0, .hash = 0, .expr = nullptr, .deps = nullptr};

  __taint::option res = __union_table.lookup(label_info);
  if (res != __taint::none()) {
    dfsan_label label = *res;
    AOUT("%u found\n", label);
    return label;
  }
  // for debugging
  dfsan_label l = atomic_load(&__dfsan_last_label, memory_order_relaxed);
  assert(l1 <= l && l2 <= l);

  dfsan_label label =
    atomic_fetch_add(&__dfsan_last_label, 1, memory_order_relaxed) + 1;
  dfsan_check_label(label);
  assert(label > l1 && label > l2);

  AOUT("%u = (%u, %u, %u, %u, %llu, %llu)\n", label, l1, l2, op, size, op1, op2);

  // setup a hash tree for dedup
  u32 h1 = l1 ? __dfsan_label_info[l1].hash : 0;
  u32 h2 = l2 ? __dfsan_label_info[l2].hash : 0;
  u32 h3 = op;
  h3 = (h3 << 16) | size;
  label_info.hash = xxhash(h1, h2, h3);

  internal_memcpy(&__dfsan_label_info[label], &label_info, sizeof(dfsan_label_info));
  __union_table.insert(&__dfsan_label_info[label], label);
  return label;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
dfsan_label __taint_union_load(const dfsan_label *ls, uptr n) {
  dfsan_label label0 = ls[0];
  if (label0 == kInitializingLabel) return kInitializingLabel;

  // for debugging
  // dfsan_label l = atomic_load(&__dfsan_last_label, memory_order_relaxed);
  // assert(label0 <= l);
  if (label0 >= CONST_OFFSET) assert(get_label_info(label0)->size != 0);

  // fast path 1: constant
  if (is_constant_label(label0)) {
    bool constant = true;
    for (uptr i = 1; i < n; i++) {
      if (!is_constant_label(ls[i])) {
        constant = false;
        break;
      }
    }
    if (constant) return CONST_LABEL;
  }
  AOUT("label0 = %d, n = %d, ls = %p\n", label0, n, ls);

  // shape
  bool shape = true;
  if (__dfsan_label_info[label0].op != 0) {
    // not raw input bytes
    shape = false;
  } else {
    off_t offset = get_label_info(label0)->op1;
    for (uptr i = 1; i != n; ++i) {
      dfsan_label next_label = ls[i];
      if (next_label == kInitializingLabel) return kInitializingLabel;
      else if (get_label_info(next_label)->op1 != offset + i) {
        shape = false;
        break;
      }
    }
  }
  if (shape) {
    if (n == 1) return label0;

    AOUT("shape: label0: %d %d\n", label0, n);
    return __taint_union(label0, (dfsan_label)n, Load, n * 8, 0, 0);
  }

  // fast path 2: all labels are extracted from a n-size label, then return that label
  if (is_kind_of_label(label0, Extract)) {
    dfsan_label parent = get_label_info(label0)->l1;
    uptr offset = 0;
    for (uptr i = 0; i < n; i++) {
      dfsan_label_info *info = get_label_info(ls[i]);
      if (!is_kind_of_label(ls[i], Extract)
            || offset != info->op2
            || parent != info->l1) {
        break;
      }
      offset += info->size;
    }
    if (get_label_info(parent)->size == offset && offset == n * 8) {
      AOUT("Fast path (2): all labels are extracts: %u\n", parent);
      return parent;
    }
  }

  // slowpath
  AOUT("union load slowpath at %p\n", __builtin_return_address(0));
  dfsan_label label = label0;
  for (uptr i = get_label_info(label0)->size / 8; i < n;) {
    dfsan_label next_label = ls[i];
    u16 next_size = get_label_info(next_label)->size;
    AOUT("next label=%u, size=%u\n", next_label, next_size);
    if (!is_constant_label(next_label)) {
      if (next_size <= (n - i) * 8) {
        i += next_size / 8;
        label = __taint_union(label, next_label, Concat, i * 8, 0, 0);
      } else {
        Report("WARNING: partial loading expected=%d has=%d\n", n-i, next_size);
        uptr size = n - i;
        dfsan_label trunc = __taint_union(next_label, CONST_LABEL, Trunc, size * 8, 0, 0);
        return __taint_union(label, trunc, Concat, n * 8, 0, 0);
      }
    } else {
      Report("WARNING: taint mixed with concrete %d\n", i);
      char *c = (char *)app_for(&ls[i]);
      ++i;
      label = __taint_union(label, 0, Concat, i * 8, 0, *c);
    }
  }
  AOUT("\n");
  return label;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __taint_union_store(dfsan_label l, dfsan_label *ls, uptr n) {
  //AOUT("label = %d, n = %d, ls = %p\n", l, n, ls);
  if (l != kInitializingLabel) {
    // for debugging
    dfsan_label h = atomic_load(&__dfsan_last_label, memory_order_relaxed);
    assert(l <= h);
  } else {
    for (uptr i = 0; i < n; ++i)
      ls[i] = l;
    return;
  }

  // fast path 1: constant
  if (l == 0) {
    for (uptr i = 0; i < n; ++i)
      ls[i] = l;
    return;
  }

  dfsan_label_info *info = get_label_info(l);
  // fast path 2: single byte
  if (n == 1 && info->size == 8) {
    ls[0] = l;
    return;
  }

  // fast path 3: load
  if (is_kind_of_label(l, Load)) {
    // if source label is union load, just break it up
    dfsan_label label0 = info->l1;
    if (n > info->l2) {
      Report("WARNING: store size=%u larger than load size=%d\n", n, info->l2);
    }
    for (uptr i = 0; i < n; ++i)
      ls[i] = label0 + i;
    return;
  }

  // default fall through
  for (uptr i = 0; i < n; ++i) {
    ls[i] = __taint_union(l, CONST_LABEL, Extract, 8, 0, i * 8);
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void dfsan_store_label(dfsan_label l, void *addr, uptr size) {
  if (l == 0) return;
  __taint_union_store(l, shadow_for(addr), size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __dfsan_unimplemented(char *fname) {
  if (flags().warn_unimplemented)
    Report("WARNING: DataFlowSanitizer: call to uninstrumented function %s\n",
           fname);

}

// Use '-mllvm -dfsan-debug-nonzero-labels' and break on this function
// to try to figure out where labels are being introduced in a nominally
// label-free program.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void __dfsan_nonzero_label() {
  if (flags().warn_nonzero_labels)
    Report("WARNING: DataFlowSanitizer: saw nonzero label\n");
}

// Indirect call to an uninstrumented vararg function. We don't have a way of
// handling these at the moment.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
__dfsan_vararg_wrapper(const char *fname) {
  Report("FATAL: DataFlowSanitizer: unsupported indirect call to vararg "
         "function %s\n", fname);
  Die();
}

// Like __dfsan_union, but for use from the client or custom functions.  Hence
// the equality comparison is done here before calling __dfsan_union.
SANITIZER_INTERFACE_ATTRIBUTE dfsan_label
dfsan_union(dfsan_label l1, dfsan_label l2, u16 op, u8 size, u64 op1, u64 op2) {
  return __taint_union(l1, l2, op, size, op1, op2);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
dfsan_label dfsan_create_label(off_t offset) {
  dfsan_label label =
    atomic_fetch_add(&__dfsan_last_label, 1, memory_order_relaxed) + 1;
  dfsan_check_label(label);
  internal_memset(&__dfsan_label_info[label], 0, sizeof(dfsan_label_info));
  __dfsan_label_info[label].size = 8;
  // label may not equal to offset when using stdin
  __dfsan_label_info[label].op1 = offset;
  return label;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __dfsan_set_label(dfsan_label label, void *addr, uptr size) {
  for (dfsan_label *labelp = shadow_for(addr); size != 0; --size, ++labelp) {
    // Don't write the label if it is already the value we need it to be.
    // In a program where most addresses are not labeled, it is common that
    // a page of shadow memory is entirely zeroed.  The Linux copy-on-write
    // implementation will share all of the zeroed pages, making a copy of a
    // page when any value is written.  The un-sharing will happen even if
    // the value written does not change the value in memory.  Avoiding the
    // write when both |label| and |*labelp| are zero dramatically reduces
    // the amount of real memory used by large programs.
    if (label == *labelp)
      continue;

    //AOUT("%p = %u\n", addr, label);
    *labelp = label;
  }
}

SANITIZER_INTERFACE_ATTRIBUTE
void dfsan_set_label(dfsan_label label, void *addr, uptr size) {
  __dfsan_set_label(label, addr, size);
}

SANITIZER_INTERFACE_ATTRIBUTE
void dfsan_add_label(dfsan_label label, u8 op, void *addr, uptr size) {
  for (dfsan_label *labelp = shadow_for(addr); size != 0; --size, ++labelp)
    *labelp = __taint_union(*labelp, label, op, 1, 0, 0);
}

// Unlike the other dfsan interface functions the behavior of this function
// depends on the label of one of its arguments.  Hence it is implemented as a
// custom function.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE dfsan_label
__dfsw_dfsan_get_label(long data, dfsan_label data_label,
                       dfsan_label *ret_label) {
  *ret_label = 0;
  return data_label;
}

SANITIZER_INTERFACE_ATTRIBUTE dfsan_label
dfsan_read_label(const void *addr, uptr size) {
  if (size == 0)
    return 0;
  return __taint_union_load(shadow_for(addr), size);
}

SANITIZER_INTERFACE_ATTRIBUTE dfsan_label
dfsan_get_label(const void *addr) {
  return *shadow_for(addr);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
const struct dfsan_label_info *dfsan_get_label_info(dfsan_label label) {
  return &__dfsan_label_info[label];
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
dfsan_has_label(dfsan_label label, dfsan_label elem) {
  if (label == elem)
    return true;
  const dfsan_label_info *info = dfsan_get_label_info(label);
  if (info->l1 != 0) {
    return dfsan_has_label(info->l1, elem);
  }
  if (info->l2 != 0) {
    return dfsan_has_label(info->l2, elem);
  } 
  return false;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE uptr
dfsan_get_label_count(void) {
  dfsan_label max_label_allocated =
      atomic_load(&__dfsan_last_label, memory_order_relaxed);

  return static_cast<uptr>(max_label_allocated);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
dfsan_dump_labels(int fd) {
  dfsan_label last_label =
      atomic_load(&__dfsan_last_label, memory_order_relaxed);

  for (uptr l = 1; l <= last_label; ++l) {
    char buf[64];
    internal_snprintf(buf, sizeof(buf), "%u (%u %u %u %u)", l,
                      __dfsan_label_info[l].l1, __dfsan_label_info[l].l2,
                      __dfsan_label_info[l].op, __dfsan_label_info[l].size);
    WriteToFile(fd, buf, internal_strlen(buf));
    WriteToFile(fd, "\n", 1);
  }
}

static z3::expr read_concrete(u64 addr, u8 size) {
  u8 *ptr = reinterpret_cast<u8*>(addr);
  if (ptr == nullptr) {
    throw z3::exception("invalid concrete address");
  }

  z3::expr val = __z3_context.bv_val(*ptr++, 8);
  for (u8 i = 1; i < size; i++) {
    val = z3::concat(__z3_context.bv_val(*ptr++, 8), val);
  }
  return val;
}

static z3::expr get_cmd(z3::expr const &lhs, z3::expr const &rhs, u32 predicate) {
  switch (predicate) {
    case bveq:  return lhs == rhs;
    case bvneq: return lhs != rhs;
    case bvugt: return z3::ugt(lhs, rhs);
    case bvuge: return z3::uge(lhs, rhs);
    case bvult: return z3::ult(lhs, rhs);
    case bvule: return z3::ule(lhs, rhs);
    case bvsgt: return lhs > rhs;
    case bvsge: return lhs >= rhs;
    case bvslt: return lhs < rhs;
    case bvsle: return lhs <= rhs;
    default:
      Printf("FATAL: unsupported predicate: %u\n", predicate);
      throw z3::exception("unsupported predicate");
      break;
  }
  // should never reach here
  Die();
}

static inline z3::expr cache_expr(dfsan_label_info *info, z3::expr const &e, std::unordered_set<u32> &deps) {
  info->expr = new z3::expr(e);
  info->deps = new std::unordered_set<u32>(deps);
  return e;
}

static z3::expr serialize(dfsan_label label, std::unordered_set<u32> &deps) {
  if (label < CONST_OFFSET || label == kInitializingLabel) {
    Report("WARNING: invalid label: %d\n", label);
    throw z3::exception("invalid label");
  }

  dfsan_label_info *info = get_label_info(label);
  AOUT("%u = (l1:%u, l2:%u, op:%u, size:%u, op1:%llu, op2:%llu)\n",
       label, info->l1, info->l2, info->op, info->size, info->op1, info->op2);

  if (info->expr) {
    auto d = reinterpret_cast<std::unordered_set<u32>*>(info->deps);
    deps.insert(d->begin(), d->end());
    return *reinterpret_cast<z3::expr*>(info->expr);
  }

  // special ops
  if (info->op == 0) {
    // input
    z3::symbol symbol = __z3_context.int_symbol(info->op1);
    z3::sort sort = __z3_context.bv_sort(8);
    info->tree_size = 1; // lazy init
    deps.insert(info->op1);
    // caching is not super helpful
    return __z3_context.constant(symbol, sort);
  } else if (info->op == Load) {
    u64 offset = get_label_info(info->l1)->op1;
    z3::symbol symbol = __z3_context.int_symbol(offset);
    z3::sort sort = __z3_context.bv_sort(8);
    z3::expr out = __z3_context.constant(symbol, sort);
    deps.insert(offset);
    for (u32 i = 1; i < info->l2; i++) {
      symbol = __z3_context.int_symbol(offset + i);
      out = z3::concat(__z3_context.constant(symbol, sort), out);
      deps.insert(offset + i);
    }
    info->tree_size = 1; // lazy init
    return cache_expr(info, out, deps);
  } else if (info->op == ZExt) {
    z3::expr base = serialize(info->l1, deps);
    if (base.is_bool()) // dirty hack since llvm lacks bool
      base = z3::ite(base, __z3_context.bv_val(1, 1),
                           __z3_context.bv_val(0, 1));
    u32 base_size = base.get_sort().bv_size();
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(info, z3::zext(base, info->size - base_size), deps);
  } else if (info->op == SExt) {
    z3::expr base = serialize(info->l1, deps);
    u32 base_size = base.get_sort().bv_size();
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(info, z3::sext(base, info->size - base_size), deps);
  } else if (info->op == Trunc) {
    z3::expr base = serialize(info->l1, deps);
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(info, base.extract(info->size - 1, 0), deps);
  } else if (info->op == Extract) {
    z3::expr base = serialize(info->l1, deps);
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(info, base.extract((info->op2 + info->size) - 1, info->op2), deps);
  } else if (info->op == Not) {
    if (info->l2 == 0 || info->size != 1) {
      throw z3::exception("invalid Not operation");
    }
    z3::expr e = serialize(info->l2, deps);
    info->tree_size = get_label_info(info->l2)->tree_size; // lazy init
    if (!e.is_bool()) {
      throw z3::exception("Only LNot should be recorded");
    }
    return cache_expr(info, !e, deps);
  } else if (info->op == Neg) {
    if (info->l2 == 0) {
      throw z3::exception("invalid Neg predicate");
    }
    z3::expr e = serialize(info->l2, deps);
    info->tree_size = get_label_info(info->l2)->tree_size; // lazy init
    return cache_expr(info, -e, deps);
  }
  // higher-order
  else if (info->op == fmemcmp) {
    z3::expr op1 = (info->l1 >= CONST_OFFSET) ? serialize(info->l1, deps) :
                   read_concrete(info->op1, info->size); // memcmp size in bytes
    if (info->l2 < CONST_OFFSET) {
      throw z3::exception("invalid memcmp operand2");
    }
    z3::expr op2 = serialize(info->l2, deps);
    info->tree_size = 1; // lazy init
    // don't cache becaue of read_concrete?
    return z3::ite(op1 == op2, __z3_context.bv_val(0, 32),
                               __z3_context.bv_val(1, 32));
  } else if (info->op == fsize) {
    // file size
    z3::symbol symbol = __z3_context.str_symbol("fsize");
    z3::sort sort = __z3_context.bv_sort(info->size);
    z3::expr base = __z3_context.constant(symbol, sort);
    info->tree_size = 1; // lazy init
    // don't cache because of deps
    if (info->op1) {
      // minus the offset stored in op1
      z3::expr offset = __z3_context.bv_val((uint64_t)info->op1, info->size);
      return base - offset;
    } else {
      return base;
    }
  }

  // common ops
  u8 size = info->size;
  // size for concat is a bit complicated ...
  if (info->op == Concat && info->l1 == 0) {
    assert(info->l2 >= CONST_OFFSET);
    size = info->size - get_label_info(info->l2)->size;
  }
  z3::expr op1 = __z3_context.bv_val((uint64_t)info->op1, size);
  if (info->l1 >= CONST_OFFSET) {
    op1 = serialize(info->l1, deps).simplify();
  } else if (info->size == 1) {
    op1 = __z3_context.bool_val(info->op1 == 1);
  }
  if (info->op == Concat && info->l2 == 0) {
    assert(info->l1 >= CONST_OFFSET);
    size = info->size - get_label_info(info->l1)->size;
  }
  z3::expr op2 = __z3_context.bv_val((uint64_t)info->op2, size);
  if (info->l2 >= CONST_OFFSET) {
    std::unordered_set<u32> deps2;
    op2 = serialize(info->l2, deps2).simplify();
    deps.insert(deps2.begin(),deps2.end());
  } else if (info->size == 1) {
    op2 = __z3_context.bool_val(info->op2 == 1);
  }
  // update tree_size
  info->tree_size = get_label_info(info->l1)->tree_size +
                    get_label_info(info->l2)->tree_size;

  switch((info->op & 0xff)) {
    // llvm doesn't distinguish between logical and bitwise and/or/xor
    case And:     return cache_expr(info, info->size != 1 ? (op1 & op2) : (op1 && op2), deps);
    case Or:      return cache_expr(info, info->size != 1 ? (op1 | op2) : (op1 || op2), deps);
    case Xor:     return cache_expr(info, op1 ^ op2, deps);
    case Shl:     return cache_expr(info, z3::shl(op1, op2), deps);
    case LShr:    return cache_expr(info, z3::lshr(op1, op2), deps);
    case AShr:    return cache_expr(info, z3::ashr(op1, op2), deps);
    case Add:     return cache_expr(info, op1 + op2, deps);
    case Sub:     return cache_expr(info, op1 - op2, deps);
    case Mul:     return cache_expr(info, op1 * op2, deps);
    case UDiv:    return cache_expr(info, z3::udiv(op1, op2), deps);
    case SDiv:    return cache_expr(info, op1 / op2, deps);
    case URem:    return cache_expr(info, z3::urem(op1, op2), deps);
    case SRem:    return cache_expr(info, z3::srem(op1, op2), deps);
    // relational
    case ICmp:    return cache_expr(info, get_cmd(op1, op2, info->op >> 8), deps);
    // concat
    case Concat:  return cache_expr(info, z3::concat(op2, op1), deps); // little endian
    default:
      Printf("FATAL: unsupported op: %u\n", info->op);
      throw z3::exception("unsupported operator");
      break;
  }
  // should never reach here
  Die();
}

static void generate_input(z3::model &m) {
  char path[PATH_MAX];
  internal_snprintf(path, PATH_MAX, "%s/id-%d-%d-%d", __output_dir,
                    __instance_id, __session_id, __current_index++);
  fd_t fd = OpenFile(path, WrOnly);
  if (fd == kInvalidFd) {
    throw z3::exception("failed to open new input file for write");
  }

  if (!tainted.is_stdin) {
    if (!WriteToFile(fd, tainted.buf, tainted.size)) {
      throw z3::exception("failed to copy original input\n");
    }
  } else {
    // FIXME: input is stdin
    throw z3::exception("original input is stdin");
  }
  AOUT("generate #%d output\n", __current_index - 1);

  // from qsym
  unsigned num_constants = m.num_consts();
  for (unsigned i = 0; i < num_constants; i++) {
    z3::func_decl decl = m.get_const_decl(i);
    z3::expr e = m.get_const_interp(decl);
    z3::symbol name = decl.name();

    if (name.kind() == Z3_INT_SYMBOL) {
      int offset = name.to_int();
      u8 value = (u8)e.get_numeral_int();
      AOUT("offset %lld = %x\n", offset, value);
      internal_lseek(fd, offset, SEEK_SET);
      WriteToFile(fd, &value, sizeof(value));
    } else { // string symbol
      if (!name.str().compare("fsize")) {
        off_t size = (off_t)e.get_numeral_int64();
        if (size > tainted.size) { // grow
          internal_lseek(fd, size, SEEK_SET);
          u8 dummy = 0;
          WriteToFile(fd, &dummy, sizeof(dummy));
        } else {
          AOUT("truncate file to %lld\n", size);
          internal_ftruncate(fd, size);
        }
        // don't remember size constraints
        throw z3::exception("skip fsize constraints");
      }
    }
  }

  CloseFile(fd);
}

SANITIZER_INTERFACE_ATTRIBUTE void
add_constraints(dfsan_label label) {
  if ((get_label_info(label)->flags & B_FLIPPED))
    return;

  try {
    std::unordered_set<dfsan_label> inputs;
    z3::expr cond = serialize(label, inputs);
    for (auto off : inputs) {
      auto c = get_branch_dep(off);
      if (c == nullptr) {
        c = new branch_dep_t();
        set_branch_dep(off, c);
      }
      if (c == nullptr) {
        Report("WARNING: out of memory\n");
      } else {
        c->input_deps.insert(inputs.begin(), inputs.end());
        c->expr_deps.insert(cond);
      }
    }
  } catch (z3::exception e) {
    Report("WARNING: adding constraints error: %s\n", e.msg());
  }

  get_label_info(label)->flags |= B_FLIPPED;
}

static void __solve_cond(dfsan_label label, z3::expr &result, bool add_nested, void *addr) {
  if ((get_label_info(label)->flags & B_FLIPPED))
    return;

  bool pushed = false;
  try {
    std::unordered_set<dfsan_label> inputs;
    z3::expr cond = serialize(label, inputs);

#if 0
    if (get_label_info(label)->tree_size > 50000) {
      // don't bother?
      throw z3::exception("formula too large");
    }
#endif

    // collect additional input deps
    std::vector<dfsan_label> worklist;
    worklist.insert(worklist.begin(), inputs.begin(), inputs.end());
    while (!worklist.empty()) {
      auto off = worklist.back();
      worklist.pop_back();

      auto deps = get_branch_dep(off);
      if (deps != nullptr) {
        for (auto i : deps->input_deps) {
          if (inputs.insert(i).second)
            worklist.push_back(i);
        }
      }
    }

    printf("%s\n", cond.to_string().c_str());
    __z3_solver.reset();
    printf("+++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
    /*
        //AOUT("%s\n", cond.to_string().c_str());
    __z3_solver.add(cond != result);
    z3::check_result res = __z3_solver.check();
    if (res == z3::sat) {
#if OPTIMISTIC
      z3::model m_opt = __z3_solver.get_model();
#endif
      __z3_solver.push();

      // 2. add constraints
      expr_set_t added;
      for (auto off : inputs) {
        //AOUT("adding offset %d\n", off);
        auto deps = get_branch_dep(off);
        if (deps != nullptr) {
          for (auto &expr : deps->expr_deps) {
            if (added.insert(expr).second) {
              //AOUT("adding expr: %s\n", expr.to_string().c_str());
              __z3_solver.add(expr);
            }
          }
        }
      }

      res = __z3_solver.check();
      if (res == z3::sat) {
        AOUT("branch solved\n");
        z3::model m = __z3_solver.get_model();
        generate_input(m);
      } else {
#if OPTIMISTIC
        generate_input(m_opt);
#endif
      }
    } else if (res == z3::unsat) {
      AOUT("branch not solvable @%p\n", addr);
      //AOUT("\n%s\n", __z3_solver.to_smt2().c_str());
      //AOUT("  tree_size = %d", __dfsan_label_info[label].tree_size);
    }

    // nested branch
    if (add_nested) {
      for (auto off : inputs) {
        auto c = get_branch_dep(off);
        if (c == nullptr) {
          c = new branch_dep_t();
          set_branch_dep(off, c);
        }
        if (c == nullptr) {
          Report("WARNING: out of memory\n");
        } else {
          c->input_deps.insert(inputs.begin(), inputs.end());
          c->expr_deps.insert(cond == result);
        }
      }
    }
    */
    // mark as flipped
    get_label_info(label)->flags |= B_FLIPPED;
  } catch (z3::exception e) {
    Report("WARNING: solving error: %s @%p\n", e.msg(), addr);
  }

}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
__taint_trace_cmp(dfsan_label op1, dfsan_label op2, u32 size, u32 predicate,
                  u64 c1, u64 c2) {
  if ((op1 == 0 && op2 == 0))
    return;

  void *addr = __builtin_return_address(0);
  auto itr = __branches.find({__taint_trace_callstack, addr});
  if (itr == __branches.end()) {
    itr = __branches.insert({{__taint_trace_callstack, addr}, 1}).first;
  } else if (itr->second < MAX_BRANCH_COUNT) {
    itr->second += 1;
  } else {
    return;
  }

  AOUT("solving cmp: %u %u %u %d %llu %llu @%p\n", op1, op2, size, predicate, c1, c2, addr);

  dfsan_label temp = dfsan_union(op1, op2, (predicate << 8) | ICmp, size, c1, c2);

  z3::expr bv_c1 = __z3_context.bv_val((uint64_t)c1, size);
  z3::expr bv_c2 = __z3_context.bv_val((uint64_t)c2, size);
  z3::expr result = get_cmd(bv_c1, bv_c2, predicate).simplify();

  // trace_cmp is only used in switch statement
  // only add nested constraints for the case taken
  __solve_cond(temp, result, c1 == c2, addr);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
__taint_trace_cond(dfsan_label label, u8 r) {
  if (label == 0)
    return;

  void *addr = __builtin_return_address(0);
  auto itr = __branches.find({__taint_trace_callstack, addr});
  if (itr == __branches.end()) {
    itr = __branches.insert({{__taint_trace_callstack, addr}, 1}).first;
  } else if (itr->second < MAX_BRANCH_COUNT) {
    itr->second += 1;
  } else {
    return;
  }

  AOUT("solving cond: %u %u %u %p %u\n", label, r, __taint_trace_callstack, addr, itr->second);

  z3::expr result = __z3_context.bool_val(r);
  __solve_cond(label, result, true, addr);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
__taint_trace_indcall(dfsan_label label) {
  if (label == 0)
    return;

  AOUT("tainted indirect call target: %d\n", label);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
__taint_trace_gep(dfsan_label label, u64 r) {
  if (label == 0)
    return;

  if ((get_label_info(label)->flags & B_FLIPPED))
    return;

  AOUT("tainted GEP index: %d = %lld\n", label, r);
  return;

  u8 size = get_label_info(label)->size;
  try {
    std::unordered_set<dfsan_label> inputs;
    z3::expr index = serialize(label, inputs);
    z3::expr result = __z3_context.bv_val((uint64_t)r, size);

    // collect additional input deps
    std::vector<dfsan_label> worklist;
    worklist.insert(worklist.begin(), inputs.begin(), inputs.end());
    while (!worklist.empty()) {
      auto off = worklist.back();
      worklist.pop_back();

      auto deps = get_branch_dep(off);
      if (deps != nullptr) {
        for (auto i : deps->input_deps) {
          if (inputs.insert(i).second)
            worklist.push_back(i);
        }
      }
    }

    __z3_solver.reset();
    __z3_solver.add(index > result);
    z3::check_result res = __z3_solver.check();

    //AOUT("\n%s\n", __z3_solver.to_smt2().c_str());
    if (res == z3::sat) {
#if OPTIMISTIC
      z3::model m_opt = __z3_solver.get_model();
#endif
      __z3_solver.push();

      // 2. add constraints
      expr_set_t added;
      for (auto off : inputs) {
        auto deps = get_branch_dep(off);
        if (deps != nullptr) {
          for (auto &expr : deps->expr_deps) {
            if (added.insert(expr).second) {
              __z3_solver.add(expr);
            }
          }
        }
      }

      res = __z3_solver.check();
      if (res == z3::sat) {
        AOUT("\tindex > %lld solved\n", r);
        z3::model m = __z3_solver.get_model();
        generate_input(m);
      } else {
#if OPTIMISTIC
        generate_input(m_opt);
#endif
      }
    } else if (res == z3::unsat) {
      AOUT("\tindex > %lld not possible\n", r);
    }

    // preserve
    for (auto off : inputs) {
      auto c = get_branch_dep(off);
      if (c == nullptr) {
        c = new branch_dep_t();
        set_branch_dep(off, c);
      }
      if (c == nullptr) {
        Report("WARNING: out of memory\n");
      } else {
        c->input_deps.insert(inputs.begin(), inputs.end());
        c->expr_deps.insert(index == result);
      }
    }

    // mark as visited
    get_label_info(label)->flags |= B_FLIPPED;
  } catch (z3::exception e) {
    Report("WARNING: index solving error: %s @%p\n", e.msg(), __builtin_return_address(0));
  }

}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
__taint_debug(dfsan_label op1, dfsan_label op2, int predicate,
              u32 size, u32 target) {
  if (op1 == 0 && op2 == 0) return;
}

SANITIZER_INTERFACE_ATTRIBUTE void
taint_set_file(const char *filename, int fd) {
  char path[PATH_MAX];
  realpath(filename, path);
  if (internal_strcmp(tainted.filename, path) == 0) {
    tainted.fd = fd;
    AOUT("fd:%d created\n", fd);

    __z3_solver.set("timeout", 5000U);
  }
}

SANITIZER_INTERFACE_ATTRIBUTE int
is_taint_file(const char *filename) {
  char path[PATH_MAX];
  realpath(filename, path);
  if (internal_strcmp(tainted.filename, path) == 0) {
    tainted.is_utmp = 1;
    return 1;
  }
  tainted.is_utmp = 0;
  return 0;
}

SANITIZER_INTERFACE_ATTRIBUTE off_t
taint_get_file(int fd) {
  AOUT("fd: %d\n", fd);
  AOUT("tainted.fd: %d\n", tainted.fd);
  return tainted.fd == fd ? tainted.size : 0;
}

SANITIZER_INTERFACE_ATTRIBUTE void
taint_close_file(int fd) {
  if (fd == tainted.fd) {
    AOUT("close tainted.fd: %d\n", tainted.fd);
    tainted.fd = -1;
  }
}

SANITIZER_INTERFACE_ATTRIBUTE int
is_stdin_taint(void) {
  return tainted.is_stdin;
}

// for utmp interface
SANITIZER_INTERFACE_ATTRIBUTE int
is_utmp_taint(void) {
  return tainted.is_utmp;
}

SANITIZER_INTERFACE_ATTRIBUTE void
set_utmp_offset(off_t offset) {
  tainted.offset = offset;
}

SANITIZER_INTERFACE_ATTRIBUTE off_t
get_utmp_offset() {
  return tainted.offset;
}

SANITIZER_INTERFACE_ATTRIBUTE void
taint_set_offset_label(dfsan_label label) {
  tainted.offset_label = label;
}

SANITIZER_INTERFACE_ATTRIBUTE dfsan_label
taint_get_offset_label() {
  return tainted.offset_label;
}

void Flags::SetDefaults() {
#define DFSAN_FLAG(Type, Name, DefaultValue, Description) Name = DefaultValue;
#include "dfsan_flags.inc"
#undef DFSAN_FLAG
}

static void RegisterDfsanFlags(FlagParser *parser, Flags *f) {
#define DFSAN_FLAG(Type, Name, DefaultValue, Description) \
  RegisterFlag(parser, #Name, Description, &f->Name);
#include "dfsan_flags.inc"
#undef DFSAN_FLAG
}

static void InitializeSolver() {
  __output_dir = flags().output_dir;
  __instance_id = flags().instance_id;
  __session_id = flags().session_id;
}

static void InitializeTaintFile() {
  for (long i = 1; i < CONST_OFFSET; i++) {
    // for synthesis
    dfsan_label label = dfsan_create_label(i);
    assert(label == i);
  }
  struct stat st;
  const char *filename = flags().taint_file;
  if (internal_strcmp(filename, "stdin") == 0) {
    tainted.fd = 0;
    // try to get the size, as stdin may be a file
    if (!fstat(0, &st)) {
      tainted.size = st.st_size;
      tainted.is_stdin = 0;
      // map a copy
      tainted.buf_size = RoundUpTo(st.st_size, GetPageSizeCached());
      uptr map = internal_mmap(nullptr, tainted.buf_size, PROT_READ, MAP_PRIVATE, 0, 0);
      if (internal_iserror(map)) {
        Printf("FATAL: failed to map a copy of input file\n");
        Die();
      }
      tainted.buf = reinterpret_cast<char *>(map);
    } else {
      tainted.size = 1;
      tainted.is_stdin = 1; // truly stdin
    }
  } else if (internal_strcmp(filename, "") == 0) {
    tainted.fd = -1;
  } else {
    if (!realpath(filename, tainted.filename)) {
      Report("WARNING: failed to get to real path for taint file\n");
      return;
    }
    stat(filename, &st);
    tainted.size = st.st_size;
    tainted.is_stdin = 0;
    // map a copy
    tainted.buf = static_cast<char *>(
      MapFileToMemory(filename, &tainted.buf_size));
    if (tainted.buf == nullptr) {
      Printf("FATAL: failed to map a copy of input file\n");
      Die();
    }
    AOUT("%s %lld size\n", filename, tainted.size);
  }

  if (tainted.fd != -1 && !tainted.is_stdin) {
    for (off_t i = 0; i < tainted.size; i++) {
      dfsan_label label = dfsan_create_label(i);
      dfsan_check_label(label);
    }
  }

  // create branch dependencies
  __branch_deps = new std::vector<branch_dep_t*>(tainted.size, nullptr);
}

static void InitializeFlags() {
  SetCommonFlagsDefaults();
  flags().SetDefaults();

  FlagParser parser;
  RegisterCommonFlags(&parser);
  RegisterDfsanFlags(&parser, &flags());
  parser.ParseString(GetEnv("TAINT_OPTIONS"));
  InitializeCommonFlags();
  if (Verbosity()) ReportUnrecognizedFlags();
  if (common_flags()->help) parser.PrintFlagDescriptions();
}

static void InitializePlatformEarly() {
  AvoidCVE_2016_2143();
#ifdef DFSAN_RUNTIME_VMA
  __dfsan::vmaSize =
    (MostSignificantSetBitIndex(GET_CURRENT_FRAME()) + 1);
  if (__dfsan::vmaSize == 39 || __dfsan::vmaSize == 42 ||
      __dfsan::vmaSize == 48) {
    __dfsan_shadow_ptr_mask = ShadowMask();
  } else {
    Printf("FATAL: DataFlowSanitizer: unsupported VMA range\n");
    Printf("FATAL: Found %d - Supported 39, 42, and 48\n", __dfsan::vmaSize);
    Die();
  }
#endif
}

static void dfsan_fini() {
  if (internal_strcmp(flags().dump_labels_at_exit, "") != 0) {
    fd_t fd = OpenFile(flags().dump_labels_at_exit, WrOnly);
    if (fd == kInvalidFd) {
      Report("WARNING: DataFlowSanitizer: unable to open output file %s\n",
             flags().dump_labels_at_exit);
      return;
    }

    Report("INFO: DataFlowSanitizer: dumping labels to %s\n",
           flags().dump_labels_at_exit);
    dfsan_dump_labels(fd);
    CloseFile(fd);
  }
  if (tainted.buf) {
    UnmapOrDie(tainted.buf, tainted.buf_size);
  }
  // write output
  char *afl_shmid = getenv("__AFL_SHM_ID");
  if (afl_shmid) {
    u32 shm_id = atoi(afl_shmid);
    void *trace_id = shmat(shm_id, NULL, 0);
    *(reinterpret_cast<u32*>(trace_id)) = __current_index;
    shmdt(trace_id);
  }
}

static void dfsan_init(int argc, char **argv, char **envp) {
  InitializeFlags();

  InitializePlatformEarly();
  MmapFixedNoReserve(ShadowAddr(), UnusedAddr() - ShadowAddr());
  __dfsan_label_info = (dfsan_label_info *)UnionTableAddr();
  // init const size
  __dfsan_label_info[CONST_LABEL].size = 8;

  InitializeInterceptors();

  // Protect the region of memory we don't use, to preserve the one-to-one
  // mapping from application to shadow memory.
  MmapFixedNoAccess(UnusedAddr(), AppAddr() - UnusedAddr());
  MmapFixedNoReserve(HashTableAddr(), hashtable_size);
  __taint::allocator_init(HashTableAddr(), HashTableAddr() + hashtable_size);

  print_debug = flags().debug;

  InitializeTaintFile();

  InitializeSolver();

  // Register the fini callback to run when the program terminates successfully
  // or it is killed by the runtime.
  Atexit(dfsan_fini);
  AddDieCallback(dfsan_fini);
}

#if SANITIZER_CAN_USE_PREINIT_ARRAY
__attribute__((section(".preinit_array"), used))
static void (*dfsan_init_ptr)(int, char **, char **) = dfsan_init;
#endif
