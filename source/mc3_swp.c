/* mc3_swp.c -- the game's SWP instructions, rewritten for ARMv8.
 *
 * libModernCombat3.so's spinlocks (119 sites, its sound and streaming
 * threads among them) swap with SWP Rd, Rm, [Rn], an ARMv6-era instruction:
 * ARMv7 cores (the PS Vita's Cortex-A9) still run it, but ARMv8's AArch32
 * has none -- on the Switch's Cortex-A57 it is an undefined instruction
 * (Linux emulates it in the kernel; Horizon does not), and the game dies
 * at the first one, a frame into its loop.
 *
 * Each SWP becomes a branch to a stub doing the same with LDREX/STREX:
 *
 *   push  {Rs, Rf}          two scratch registers (not Rd, Rn, Rm)
 *   mrs   Rf, APSR          SWP leaves the flags alone; the loop's cmp must too
 *   dmb   ish
 * 1:ldrex Rd, [Rn]          the old value, as SWP returns it
 *   strex Rs, Rm, [Rn]      the new one
 *   cmp   Rs, #0
 *   bne   1b                lost the reservation: again
 *   dmb   ish
 *   msr   APSR_nzcvq, Rf
 *   pop   {Rs, Rf}
 *   b     <the next instruction>
 *
 * The stubs go where the branch reaches them (B: +-32 MB) and the code is
 * executable: over .rel.dyn, which lies in the module's own RX segment a
 * few MB below .text and is dead once so_relocate/so_resolve have run (the
 * loader reads it in those two only). So mc3_swp_fix runs between
 * so_resolve and so_finalize, on the staged image.
 *
 * Which words are code: a word is taken only when it is an unconditional
 * SWP (cond AL; all 119 are), its registers are distinct and none is SP or
 * PC, and it is not the target of a pc-relative LDR (a literal that merely
 * looks like one). MIT.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef MC3_SWP_HOST_TEST
#include "so_util.h"
#include "util.h"
#include "mc3.h"
#else
#include <stdio.h>
#define debugPrintf printf
#endif

#define STUB_WORDS 11

static int is_swp(uint32_t w) { return (w & 0xFFF00FF0u) == 0xE1000090u; } /* SWP, cond AL (not SWPB) */
static int is_ldr_literal(uint32_t w) { return (w & 0x0F7F0000u) == 0x051F0000u && (w >> 28) != 0xF; }

static uint32_t arm_b(uint32_t cond, uint32_t from, uint32_t to) {
  return (cond << 28) | 0x0A000000u | (((to - (from + 8)) >> 2) & 0x00FFFFFFu);
}

static int in_range(uint32_t from, uint32_t to) {
  int64_t d = (int64_t)to - (int64_t)(from + 8);
  return d >= -(int64_t)0x2000000 && d < (int64_t)0x2000000;
}

/* img: the module image, its vaddr 0 at img[0]. text/pool: vaddr ranges.
 * Returns the sites rewritten, -1 when the pool is too small. */
int mc3_swp_fix_image(uint8_t *img, uint32_t text_va, uint32_t text_size, uint32_t pool_va,
                      uint32_t pool_size, uint32_t *pool_used) {
  uint32_t *text = (uint32_t *)(img + text_va);
  uint32_t nw = text_size / 4;
  uint8_t *lit = calloc((nw + 7) / 8, 1); /* words read as literals */
  if (!lit)
    return -1;
  for (uint32_t i = 0; i < nw; i++) {
    uint32_t w = text[i];
    if (!is_ldr_literal(w))
      continue;
    uint32_t imm = w & 0xFFF;
    int64_t a = (int64_t)(text_va + i * 4 + 8) + (((w >> 23) & 1) ? imm : -(int64_t)imm);
    if (a >= text_va && a < (int64_t)text_va + text_size && !(a & 3)) {
      uint32_t k = (uint32_t)(a - text_va) / 4;
      lit[k / 8] |= (uint8_t)(1u << (k % 8));
    }
  }

  uint32_t *pool = (uint32_t *)(img + pool_va);
  uint32_t used = 0, fixed = 0, skipped = 0;
  for (uint32_t i = 0; i < nw; i++) {
    uint32_t w = text[i];
    if (!is_swp(w))
      continue;
    uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, rm = w & 15;
    uint32_t site = text_va + i * 4;
    if ((lit[i / 8] >> (i % 8)) & 1 || rd == rn || rd == rm || rn == rm || rn >= 13 || rd >= 13 ||
        rm >= 13) {
      debugPrintf("[swp] left alone at +0x%x (0x%08x)\n", (unsigned)site, (unsigned)w);
      skipped++;
      continue;
    }
    if ((used + STUB_WORDS) * 4 > pool_size) {
      free(lit);
      return -1;
    }
    uint32_t stub = pool_va + used * 4;
    if (!in_range(site, stub) || !in_range(stub + (STUB_WORDS - 1) * 4, site + 4)) {
      debugPrintf("[swp] +0x%x: the pool is out of branch range\n", (unsigned)site);
      skipped++;
      continue;
    }
    uint32_t rs = 0, rf;
    while (rs == rd || rs == rn || rs == rm)
      rs++;
    rf = rs + 1;
    while (rf == rd || rf == rn || rf == rm)
      rf++;
    uint32_t mask = (1u << rs) | (1u << rf);
    uint32_t *s = pool + used;
    s[0] = 0xE92D0000u | mask;                                /* push  {rs, rf}        */
    s[1] = 0xE10F0000u | (rf << 12);                          /* mrs   rf, APSR        */
    s[2] = 0xF57FF05Bu;                                       /* dmb   ish             */
    s[3] = 0xE1900F9Fu | (rn << 16) | (rd << 12);             /* ldrex rd, [rn]        */
    s[4] = 0xE1800F90u | (rn << 16) | (rs << 12) | rm;        /* strex rs, rm, [rn]    */
    s[5] = 0xE3500000u | (rs << 16);                          /* cmp   rs, #0          */
    s[6] = arm_b(0x1, stub + 6 * 4, stub + 3 * 4);            /* bne   ldrex           */
    s[7] = 0xF57FF05Bu;                                       /* dmb   ish             */
    s[8] = 0xE128F000u | rf;                                  /* msr   APSR_nzcvq, rf  */
    s[9] = 0xE8BD0000u | mask;                                /* pop   {rs, rf}        */
    s[10] = arm_b(0xE, stub + 10 * 4, site + 4);              /* b     next            */
    text[i] = arm_b(0xE, site, stub);
    used += STUB_WORDS;
    fixed++;
  }
  free(lit);
  if (pool_used)
    *pool_used = used * 4;
  if (skipped)
    debugPrintf("[swp] %u SWP left alone (see above)\n", (unsigned)skipped);
  return (int)fixed;
}

#ifndef MC3_SWP_HOST_TEST
static uint32_t g_pool_lo, g_pool_hi; /* runtime addresses, for crash reports */

static const Elf32_Shdr *section(so_module *m, const char *name) {
  for (int i = 0; i < m->elf_hdr->e_shnum; i++)
    if (!strcmp(m->shstrtab + m->sec_hdr[i].sh_name, name))
      return &m->sec_hdr[i];
  return NULL;
}

/* Between so_resolve and so_finalize. 0, or -1 (logged) when it could not. */
int mc3_swp_fix(so_module *m) {
  if (m->state != SO_STAGED) {
    debugPrintf("[swp] %s is not staged: SWP not rewritten\n", m->base_name);
    return -1;
  }
  const Elf32_Shdr *text = section(m, ".text"), *rel = section(m, ".rel.dyn");
  if (!text || !rel) {
    debugPrintf("[swp] %s: no .text or .rel.dyn\n", m->base_name);
    return -1;
  }
  uint32_t used = 0;
  int n = mc3_swp_fix_image((uint8_t *)m->load_base, text->sh_addr, text->sh_size, rel->sh_addr,
                            rel->sh_size, &used);
  if (n < 0) {
    debugPrintf("[swp] %s: the stub pool (.rel.dyn, %u bytes) is too small\n", m->base_name,
                (unsigned)rel->sh_size);
    return -1;
  }
  g_pool_lo = (uint32_t)(uintptr_t)m->load_virtbase + rel->sh_addr;
  g_pool_hi = g_pool_lo + used;
  debugPrintf("[swp] %s: %d SWP -> LDREX/STREX stubs (%u bytes over .rel.dyn)\n", m->base_name, n,
              (unsigned)used);
  return 0;
}

/* exc_handler.c: names our stubs in crash reports. */
const char *port_code_region(uint32_t addr) {
  return addr >= g_pool_lo && addr < g_pool_hi ? "mc3 SWP stub" : NULL;
}
#endif
