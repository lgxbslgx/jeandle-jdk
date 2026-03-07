/*
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
 */


#include "jeandle/__llvmHeadersBegin__.hpp"
#include "llvm/BinaryFormat/Dwarf.h"

#include "jeandle/jeandleAssembler.hpp"
#include "jeandle/jeandleCompiledCode.hpp"
#include "jeandle/jeandleCompilation.hpp"
#include "jeandle/jeandleRegister.hpp"
#include "jeandle/jeandleReloc.hpp"
#include "jeandle/jeandleRuntimeRoutine.hpp"

// Get the frame size from .stack_sizes section.
void JeandleCompiledCode::setup_frame_size() {
  SectionInfo section_info(".stack_sizes");
  bool found = ReadELF::findSection(*_elf, section_info);
  JEANDLE_ERROR_ASSERT_AND_RET_VOID_ON_FAIL(found, ".stack_sizes section not found");

  llvm::DataExtractor data_extractor(llvm::StringRef(((char*)_obj->getBufferStart()) + section_info._offset, section_info._size),
                                     true/* IsLittleEndian */, BytesPerWord/* AddressSize */);
  uint64_t offset = 0;
  data_extractor.getUnsigned(&offset, BytesPerWord);
  uint64_t frame_size_in_bytes = data_extractor.getULEB128(&offset);
  assert(frame_size_in_bytes % StackAlignmentInBytes == 0, "frame size must be aligned");
  _frame_size = frame_size_in_bytes / BytesPerWord;
}

void JeandleCompiledCode::build_exception_handler_table() {
  SectionInfo excpet_table_section(".gcc_except_table");
  if (ReadELF::findSection(*_elf, excpet_table_section)) {
    // Start of the exception handler table.
    const char* except_table_pointer = object_start() + excpet_table_section._offset;

    // Utilize DataExtractor to decode exception handler table.
    llvm::DataExtractor data_extractor(llvm::StringRef(except_table_pointer, excpet_table_section._size),
                                       ELFT::Endianness == llvm::endianness::little, /* IsLittleEndian */
                                       BytesPerWord/* AddressSize */);
    llvm::DataExtractor::Cursor data_cursor(0 /* Offset */);

    // Now decode exception handler table.
    // See EHStreamer::emitExceptionTable in Jeandle-LLVM for corresponding encoding.

    uint8_t header_encoding = data_extractor.getU8(data_cursor);
    assert(data_cursor && header_encoding == llvm::dwarf::DW_EH_PE_omit, "invalid exception handler table header");

    uint8_t type_encoding = data_extractor.getU8(data_cursor);
    assert(data_cursor && type_encoding == llvm::dwarf::DW_EH_PE_omit, "invalid exception handler table type encoding");

    uint8_t call_site_encoding = data_extractor.getU8(data_cursor);
    assert(data_cursor && call_site_encoding == llvm::dwarf::DW_EH_PE_uleb128, "invalid exception handler table call site encoding");

    uint64_t call_site_table_length = data_extractor.getULEB128(data_cursor);
    assert(data_cursor, "invalid exception handler table call site table length");

    uint64_t call_site_table_start = data_cursor.tell();

    while (data_cursor.tell() < call_site_table_start + call_site_table_length) {
      uint64_t start = data_extractor.getULEB128(data_cursor) + _prolog_length;
      assert(data_cursor, "invalid exception handler start pc");

      uint64_t length = data_extractor.getULEB128(data_cursor);
      assert(data_cursor, "invalid exception handler length");

      uint64_t langding_pad = data_extractor.getULEB128(data_cursor) + _prolog_length;
      assert(data_cursor, "invalid exception handler landing pad");

      _exception_handler_table.add_handler(start, start + length, langding_pad);

      // Read an action table entry, but we don't use it.
      data_extractor.getULEB128(data_cursor);
      assert(data_cursor, "invalid exception handler action table entry");
    }
  }
}

bool JeandleCompiledCode::pd_resolve_reloc(JeandleAssembler& assembler,
                                           llvm::SmallVector<JeandleReloc*>& relocs,
                                           llvm::jitlink::LinkGraph* link_graph) {
  return false;
}
