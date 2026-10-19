#pragma once

// PGD, the container amctrl.prx uses for DRM-protected game data.  A game opens
// such a file with PSP_O_NPDRM, passes its 16-byte version key to
// sceIoIoctl(0x04100001) and from then on reads plain data.  Some games keep a
// small PGD file purely as a check that the decryption works.
//
// Only the variants without console-specific keys are supported (DRM type 1,
// which is what UMD games use).

#include <cstdint>
#include <span>
#include <vector>

namespace pspweb::pgd {

inline constexpr std::size_t kHeaderSize = 0x90;

[[nodiscard]] bool is_pgd(std::span<const std::uint8_t> file) noexcept;

// Verifies the header MACs and decrypts the data.  `version_key` may be null,
// in which case the key is recovered from the header.  Returns false if the
// file is not PGD, uses an unsupported variant or the MACs do not match.
[[nodiscard]] bool decrypt(std::span<const std::uint8_t> file, const std::uint8_t *version_key,
                           std::vector<std::uint8_t> &plain);

} // namespace pspweb::pgd
