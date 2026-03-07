/*
 * Copyright (c) 1998, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
 * Copyright (c) 2020, 2022, Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2025, 2026, the Jeandle-JDK Authors. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "code/relocInfo.hpp"
#include "nativeInst_riscv.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/safepoint.hpp"

void Relocation::pd_set_data_value(address x, intptr_t o, bool verify_only) {
  if (verify_only) {
    return;
  }

  int bytes;

  switch (type()) {
    case relocInfo::oop_type: {
      oop_Relocation *reloc = (oop_Relocation *)this;
      // in movoop when BarrierSet::barrier_set()->barrier_set_nmethod() isn't null
      if (NativeInstruction::is_load_pc_relative_at(addr())) {
        address constptr = (address)code()->oop_addr_at(reloc->oop_index());
        bytes = MacroAssembler::pd_patch_instruction_size(addr(), constptr);
        assert((address)Bytes::get_native_u8(constptr) == x, "error in oop relocation");
      } else {
        bytes = MacroAssembler::patch_oop(addr(), x);
      }
      break;
    }
    default:
      bytes = MacroAssembler::pd_patch_instruction_size(addr(), x);
      break;
  }
  ICache::invalidate_range(addr(), bytes);
}

address Relocation::pd_call_destination(address orig_addr) {
  assert(is_call(), "should be an address instruction here");
  if (NativeCall::is_call_at(addr())) {
    address trampoline = nativeCall_at(addr())->get_trampoline();
    if (trampoline != nullptr) {
      return nativeCallTrampolineStub_at(trampoline)->destination();
    }
  }
  if (orig_addr != nullptr) {
    // the extracted address from the instructions in address orig_addr
    address new_addr = MacroAssembler::pd_call_destination(orig_addr);
    // If call is branch to self, don't try to relocate it, just leave it
    // as branch to self. This happens during code generation if the code
    // buffer expands. It will be relocated to the trampoline above once
    // code generation is complete.
    new_addr = (new_addr == orig_addr) ? addr() : new_addr;
    return new_addr;
  }
  return MacroAssembler::pd_call_destination(addr());
}

void Relocation::pd_set_call_destination(address x) {
  assert(is_call(), "should be an address instruction here");
  if (NativeCall::is_call_at(addr())) {
    address trampoline = nativeCall_at(addr())->get_trampoline();
    if (trampoline != nullptr) {
      nativeCall_at(addr())->set_destination_mt_safe(x, /* assert_lock */false);
      return;
    }
  }
  MacroAssembler::pd_patch_instruction_size(addr(), x);
  address pd_call = pd_call_destination(addr());
  assert(pd_call == x, "fail in reloc");
}

void Relocation::pd_set_jeandle_data_value(address x, int addend, bool verify_only) {
  assert(is_jeandle_reloc(), "unexpected reloc type: %d", type());
  if (verify_only) {
    return;
  }

  if (addr_in_const()) {
    assert(type() == relocInfo::jeandle_section_word_type, "Wrong relocation type");
    auto actual_relocation = (jeandle_section_word_Relocation *) this;
    int section = actual_relocation->section();
    int64_t rel_offset = actual_relocation->rel_offset();
    if (section == CodeBuffer::SECT_INSTS) {
      int new_offset = static_cast<int32_t>((int64_t)x & 0x7fffffff) - static_cast<int32_t>(rel_offset & 0x7fffffff) + addend;
      *(int*)addr() = *(int*)addr() + new_offset;
      actual_relocation->set_rel_offset((int64_t)x);
    } else {
      assert(section == CodeBuffer::SECT_CONSTS, "Wrong relocation section");
      int new_offset = static_cast<int32_t>(rel_offset & 0x7fffffff) - static_cast<int32_t>((int64_t)x & 0x7fffffff) - addend;
      *(int*)addr() = *(int*)addr() + new_offset;
      actual_relocation->set_rel_offset((int64_t)x);
    }
    return;
  }

  int64_t rel_offset = 0;
  if (type() == relocInfo::jeandle_section_word_type) {
    auto actual_relocation = (jeandle_section_word_Relocation *) this;
    rel_offset = actual_relocation->rel_offset();
  } else if (type() == relocInfo::jeandle_oop_type) {
    auto actual_relocation = (jeandle_oop_Relocation *) this;
    rel_offset = actual_relocation->rel_offset();
  } else {
    assert(type() == relocInfo::jeandle_oop_addr_type, "unexpected reloc type: %d", type());
    auto actual_relocation = (jeandle_oop_addr_Relocation *) this;
    rel_offset = actual_relocation->rel_offset();
  }

  // Handle the following three cases. Please note that load/float_load/addi may be not
  // followed by auipc immediately, so we need to identify and handle them separately.
  // 1. auipc + load
  // 2. auipc + float_load
  // 3. auipc + addi
  address insn_addr = addr();
  ptrdiff_t offset = (ptrdiff_t)((uintptr_t)x - (uintptr_t)insn_addr + addend) + rel_offset;
  if (NativeInstruction::is_auipc_at(insn_addr)) {
    Assembler::patch(insn_addr, 31, 12, ((offset + 0x800) >> 12) & 0xfffff);
  } else if (NativeInstruction::is_ld_at(insn_addr)) {
    Assembler::patch(insn_addr, 31, 20, offset & 0xfff);
  } else if (NativeInstruction::is_float_load_at(insn_addr)) {
    Assembler::patch(insn_addr, 31, 20, offset & 0xfff);
  } else if (NativeInstruction::is_addi_at(insn_addr)) {
    Assembler::patch(insn_addr, 31, 20, offset & 0xfff);
  } else {
    tty->print_cr("instruction 0x%x at " INTPTR_FORMAT " could not be patched.",
                  Assembler::ld_instr(insn_addr), p2i(insn_addr));
    ShouldNotReachHere();
  }
  ICache::invalidate_range(insn_addr, NativeInstruction::instruction_size);
}

address* Relocation::pd_address_in_code() {
  assert(NativeCall::is_load_pc_relative_at(addr()), "Not the expected instruction sequence!");
  return (address*)(MacroAssembler::target_addr_for_insn(addr()));
}

address Relocation::pd_get_address_from_code() {
  return MacroAssembler::pd_call_destination(addr());
}

void poll_Relocation::fix_relocation_after_move(const CodeBuffer* src, CodeBuffer* dest) {
  if (NativeInstruction::maybe_cpool_ref(addr())) {
    address old_addr = old_addr_for(addr(), src, dest);
    MacroAssembler::pd_patch_instruction_size(addr(), MacroAssembler::target_addr_for_insn(old_addr));
  }
}

void metadata_Relocation::pd_fix_value(address x) {
}

#ifdef JEANDLE
void trampoline_stub_Relocation::pd_fix_owner_after_move() {
  NativeCall* call = nativeCall_at(owner());
  address trampoline = addr();
  address dest = nativeCallTrampolineStub_at(trampoline)->destination();
  if (!Assembler::reachable_from_branch_at(owner(), dest)) {
    dest = trampoline;
  }
  call->set_destination(dest);
}
#endif // JEANDLE

void jeandle_oop_addr_Relocation::fix_relocation_after_move(const CodeBuffer* src, CodeBuffer* dest) {
  assert(addr_in_const(), "must in const section");
  address old_addr = *(address*)addr();
  int delta = dest - src;
  address new_addr = old_addr + delta;
  set_value(new_addr);
}
