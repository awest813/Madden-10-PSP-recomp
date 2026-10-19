// Synthetic PSP (MIPS32 little-endian) ELF executables for translator tests;
// contains only hand-assembled code, no game data.
const TEXT_BASE = 0x08804000;

// Two small functions with calls and a conditional branch.
export const SAMPLE_CODE = [
  0x27BDFFF0, // addiu sp, sp, -16
  0xAFBF000C, // sw ra, 12(sp)
  0x0E20100A, // jal 0x08804028
  0x24040003, //  addiu a0, zero, 3
  0x10400002, // beq v0, zero, +2
  0x00000000, //  nop
  0x24420001, // addiu v0, v0, 1
  0x8FBF000C, // lw ra, 12(sp)
  0x03E00008, // jr ra
  0x27BD0010, //  addiu sp, sp, 16
  0x03E00008, // 0x08804028: jr ra
  0x00841021, //  addu v0, a0, a0
];

export function pspElf(words = SAMPLE_CODE) {
  const text = new Uint8Array(words.length * 4);
  const textView = new DataView(text.buffer);
  words.forEach((word, i) => textView.setUint32(i * 4, word >>> 0, true));
  const names = new TextEncoder().encode('\0.text\0.shstrtab\0');
  const textOffset = 0x1000, namesOffset = textOffset + text.length, shOffset = (namesOffset + names.length + 3) & ~3;
  const bytes = new Uint8Array(shOffset + 3 * 40);
  const view = new DataView(bytes.buffer);
  bytes.set([0x7F, 0x45, 0x4C, 0x46, 1, 1, 1]);
  view.setUint16(16, 2, true);           // ET_EXEC
  view.setUint16(18, 8, true);           // EM_MIPS
  view.setUint32(20, 1, true);
  view.setUint32(24, TEXT_BASE, true);   // entry
  view.setUint32(28, 52, true);          // phoff
  view.setUint32(32, shOffset, true);
  view.setUint32(36, 0x10A23001, true);  // Allegrex flags
  view.setUint16(40, 52, true); view.setUint16(42, 32, true); view.setUint16(44, 1, true);
  view.setUint16(46, 40, true); view.setUint16(48, 3, true); view.setUint16(50, 2, true);
  const ph = 52;
  view.setUint32(ph, 1, true); view.setUint32(ph + 4, textOffset, true);
  view.setUint32(ph + 8, TEXT_BASE, true); view.setUint32(ph + 12, TEXT_BASE, true);
  view.setUint32(ph + 16, text.length, true); view.setUint32(ph + 20, text.length, true);
  view.setUint32(ph + 24, 5, true); view.setUint32(ph + 28, 16, true);
  bytes.set(text, textOffset);
  bytes.set(names, namesOffset);
  const section = (index, name, type, flags, address, offset, size) => {
    const at = shOffset + index * 40;
    view.setUint32(at, name, true); view.setUint32(at + 4, type, true); view.setUint32(at + 8, flags, true);
    view.setUint32(at + 12, address, true); view.setUint32(at + 16, offset, true); view.setUint32(at + 20, size, true);
    view.setUint32(at + 32, 4, true);
  };
  section(1, 1, 1, 6, TEXT_BASE, textOffset, text.length);
  section(2, 7, 3, 0, 0, namesOffset, names.length);
  return bytes;
}
