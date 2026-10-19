// PGD decryption.  The PSP does this in amctrl.prx with the KIRK crypto engine;
// everything it needs for UMD games reduces to AES-128 with three keys from
// KIRK's key vault:
//
//   BBMac: AES-CMAC with key 0x38, whitened by a constant and, for the MAC that
//          ties a file to its game, encrypted once more with the version key.
//          MAC type 3 additionally stores the MAC encrypted with key 0x63.
//   BBCipher: a counter mode.  The per-file key is turned into a counter base
//          with key 0x39, and each 16-byte counter block is CBC-decrypted with
//          key 0x63 to give the key stream.
//
// The header (0x90 bytes) carries a MAC under a fixed DNAS key at 0x80, a MAC
// under the version key at 0x70, and at 0x30 an encrypted descriptor with the
// data key, the data size, the block size and the data offset.  Reverse
// engineering of amctrl.prx by tpu and the PSP developer wiki documented all of
// this; the keys are the KIRK key vault entries.

#include "pgd.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace pspweb::pgd {
namespace {

using Block = std::array<std::uint8_t, 16>;

// --- AES-128 -----------------------------------------------------------------

std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    while (b != 0u) {
        if ((b & 1u) != 0u) product ^= a;
        a = static_cast<std::uint8_t>((a << 1) ^ ((a & 0x80u) != 0u ? 0x1Bu : 0u));
        b >>= 1;
    }
    return product;
}

struct SBoxes {
    std::uint8_t forward[256];
    std::uint8_t inverse[256];
    SBoxes() {
        for (int x = 0; x < 256; ++x) {
            // Multiplicative inverse in GF(2^8) (x^254), then the affine transform.
            std::uint8_t inv = 1;
            for (int i = 0; i < 254; ++i) inv = gf_mul(inv, static_cast<std::uint8_t>(x));
            if (x == 0) inv = 0;
            std::uint8_t s = inv;
            for (int r = 1; r <= 4; ++r) s ^= static_cast<std::uint8_t>((inv << r) | (inv >> (8 - r)));
            s ^= 0x63u;
            forward[x] = s;
            inverse[s] = static_cast<std::uint8_t>(x);
        }
    }
};

const SBoxes &sboxes() {
    static const SBoxes boxes;
    return boxes;
}

class Aes128 {
public:
    explicit Aes128(const Block &key) {
        const auto &sbox = sboxes().forward;
        std::memcpy(round_keys_.data(), key.data(), 16);
        std::uint8_t rcon = 1;
        for (std::size_t i = 16; i < round_keys_.size(); i += 4) {
            std::uint8_t t[4] = {round_keys_[i - 4], round_keys_[i - 3], round_keys_[i - 2], round_keys_[i - 1]};
            if (i % 16 == 0) {
                const std::uint8_t first = t[0];
                t[0] = static_cast<std::uint8_t>(sbox[t[1]] ^ rcon);
                t[1] = sbox[t[2]];
                t[2] = sbox[t[3]];
                t[3] = sbox[first];
                rcon = gf_mul(rcon, 2);
            }
            for (int j = 0; j < 4; ++j) round_keys_[i + j] = static_cast<std::uint8_t>(round_keys_[i - 16 + j] ^ t[j]);
        }
    }

    void encrypt(Block &b) const {
        const auto &sbox = sboxes().forward;
        add_round_key(b, 0);
        for (int round = 1; round <= 10; ++round) {
            Block s;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) s[r + 4 * c] = sbox[b[r + 4 * ((c + r) % 4)]];
            if (round != 10) {
                for (int c = 0; c < 4; ++c) {
                    const std::uint8_t *a = &s[4 * c];
                    b[4 * c + 0] = static_cast<std::uint8_t>(gf_mul(a[0], 2) ^ gf_mul(a[1], 3) ^ a[2] ^ a[3]);
                    b[4 * c + 1] = static_cast<std::uint8_t>(a[0] ^ gf_mul(a[1], 2) ^ gf_mul(a[2], 3) ^ a[3]);
                    b[4 * c + 2] = static_cast<std::uint8_t>(a[0] ^ a[1] ^ gf_mul(a[2], 2) ^ gf_mul(a[3], 3));
                    b[4 * c + 3] = static_cast<std::uint8_t>(gf_mul(a[0], 3) ^ a[1] ^ a[2] ^ gf_mul(a[3], 2));
                }
            } else {
                b = s;
            }
            add_round_key(b, round);
        }
    }

    void decrypt(Block &b) const {
        const auto &inv = sboxes().inverse;
        add_round_key(b, 10);
        for (int round = 9; round >= 0; --round) {
            Block s;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) s[r + 4 * ((c + r) % 4)] = inv[b[r + 4 * c]];
            b = s;
            add_round_key(b, round);
            if (round == 0) break;
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t a[4] = {b[4 * c], b[4 * c + 1], b[4 * c + 2], b[4 * c + 3]};
                b[4 * c + 0] = static_cast<std::uint8_t>(gf_mul(a[0], 14) ^ gf_mul(a[1], 11) ^ gf_mul(a[2], 13) ^ gf_mul(a[3], 9));
                b[4 * c + 1] = static_cast<std::uint8_t>(gf_mul(a[0], 9) ^ gf_mul(a[1], 14) ^ gf_mul(a[2], 11) ^ gf_mul(a[3], 13));
                b[4 * c + 2] = static_cast<std::uint8_t>(gf_mul(a[0], 13) ^ gf_mul(a[1], 9) ^ gf_mul(a[2], 14) ^ gf_mul(a[3], 11));
                b[4 * c + 3] = static_cast<std::uint8_t>(gf_mul(a[0], 11) ^ gf_mul(a[1], 13) ^ gf_mul(a[2], 9) ^ gf_mul(a[3], 14));
            }
        }
    }

private:
    void add_round_key(Block &b, int round) const {
        for (int i = 0; i < 16; ++i) b[i] ^= round_keys_[16 * round + i];
    }

    std::array<std::uint8_t, 176> round_keys_{};
};

// --- KIRK key vault entries and amctrl constants -----------------------------

constexpr Block kKirkKey38{0x12, 0x46, 0x8D, 0x7E, 0x1C, 0x42, 0x20, 0x9B, 0xBA, 0x54, 0x26, 0x83, 0x5E, 0xB0, 0x33, 0x03};
constexpr Block kKirkKey39{0xC4, 0x3B, 0xB6, 0xD6, 0x53, 0xEE, 0x67, 0x49, 0x3E, 0xA9, 0x5F, 0xBC, 0x0C, 0xED, 0x6F, 0x8A};
constexpr Block kKirkKey63{0x9C, 0x9B, 0x13, 0x72, 0xF8, 0xC6, 0x40, 0xCF, 0x1C, 0x62, 0xF5, 0xD5, 0x92, 0xDD, 0xB5, 0x82};
constexpr Block kDnasKey{0xED, 0xE2, 0x5D, 0x2D, 0xBB, 0xF8, 0x12, 0xE5, 0x3C, 0x5C, 0x59, 0x32, 0xFA, 0xE3, 0xE2, 0x43};
constexpr Block kMacWhitening{0xE3, 0x50, 0xED, 0x1D, 0x91, 0x0A, 0x1F, 0xD0, 0x29, 0xBB, 0x1C, 0x3E, 0xF3, 0x40, 0x77, 0xFB};
constexpr Block kCipherOut{0x13, 0x5F, 0xA4, 0x7C, 0xAB, 0x39, 0x5B, 0xA4, 0x76, 0xB8, 0xCC, 0xA9, 0x8F, 0x3A, 0x04, 0x45};
constexpr Block kCipherIn{0x67, 0x8D, 0x7F, 0xA3, 0x2A, 0x9C, 0xA0, 0xD1, 0x50, 0x8A, 0xD8, 0x38, 0x5E, 0x4B, 0x01, 0x7E};

const Aes128 &aes38() { static const Aes128 aes(kKirkKey38); return aes; }
const Aes128 &aes39() { static const Aes128 aes(kKirkKey39); return aes; }
const Aes128 &aes63() { static const Aes128 aes(kKirkKey63); return aes; }

Block operator^(Block a, const Block &b) {
    for (int i = 0; i < 16; ++i) a[i] ^= b[i];
    return a;
}

Block block_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    Block b;
    std::memcpy(b.data(), bytes.data() + offset, 16);
    return b;
}

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// CMAC subkey derivation: shift left by one bit in GF(2^128).
Block doubled(Block b) {
    const bool carry = (b[0] & 0x80u) != 0u;
    for (int i = 0; i < 15; ++i) b[i] = static_cast<std::uint8_t>((b[i] << 1) | (b[i + 1] >> 7));
    b[15] = static_cast<std::uint8_t>((b[15] << 1) ^ (carry ? 0x87u : 0u));
    return b;
}

// BBMac for MAC types 1 and 3 (type 2 needs the console's fuse key).
Block bbmac(std::span<const std::uint8_t> data, const Block *key) {
    const Aes128 &aes = aes38();
    std::size_t tail = data.size() % 16;
    if (tail == 0 && !data.empty()) tail = 16;
    Block state{};
    for (std::size_t i = 0; i + tail < data.size(); i += 16) {
        state = state ^ block_at(data, i);
        aes.encrypt(state);
    }
    Block subkey{};
    aes.encrypt(subkey);
    subkey = doubled(subkey);
    Block last{};
    std::memcpy(last.data(), data.data() + data.size() - tail, tail);
    if (tail < 16) {
        subkey = doubled(subkey);
        last[tail] = 0x80u;
    }
    state = state ^ last ^ subkey;
    aes.encrypt(state);
    state = state ^ kMacWhitening;
    if (key != nullptr) {
        state = state ^ *key;
        aes.encrypt(state);
    }
    return state;
}

// BBCipher type 1, starting with counter `seed` (1 for the first 16 bytes).
void bbcipher(const Block &key, std::uint32_t seed, std::uint8_t *data, std::size_t size) {
    Block base = key ^ kCipherIn;
    aes39().decrypt(base);
    base = base ^ kCipherOut;
    const auto counter = [&](std::uint32_t n) {
        Block c = base;
        for (int i = 0; i < 4; ++i) c[12 + i] = static_cast<std::uint8_t>(n >> (8 * i));
        return c;
    };
    Block previous = seed == 1u ? Block{} : counter(seed - 1u);
    for (std::size_t offset = 0; offset < size; offset += 16, ++seed) {
        const Block current = counter(seed);
        Block stream = current;
        aes63().decrypt(stream);
        stream = stream ^ previous;
        previous = current;
        const std::size_t n = std::min<std::size_t>(16, size - offset);
        for (std::size_t i = 0; i < n; ++i) data[offset + i] ^= stream[i];
    }
}

} // namespace

bool is_pgd(std::span<const std::uint8_t> file) noexcept {
    return file.size() >= kHeaderSize && file[0] == 0u && file[1] == 'P' && file[2] == 'G' && file[3] == 'D';
}

bool decrypt(std::span<const std::uint8_t> file, const std::uint8_t *version_key, std::vector<std::uint8_t> &plain) {
    if (!is_pgd(file)) return false;
    const std::uint32_t key_index = le32(file.data() + 4);
    const std::uint32_t drm_type = le32(file.data() + 8);
    if (drm_type != 1u) return false; // console-bound content
    const bool encrypted_macs = key_index > 1u; // MAC type 3

    const auto stored_mac = [&](std::size_t offset) {
        Block mac = block_at(file, offset);
        if (encrypted_macs) aes63().decrypt(mac);
        return mac;
    };
    if (bbmac(file.first(0x80), &kDnasKey) != stored_mac(0x80)) return false;

    Block vkey;
    if (version_key != nullptr) {
        std::memcpy(vkey.data(), version_key, 16);
        if (bbmac(file.first(0x70), &vkey) != stored_mac(0x70)) return false;
    } else {
        // The MAC at 0x70 is E(key 0x38, mac_without_key ^ vkey).
        Block target = stored_mac(0x70);
        aes38().decrypt(target);
        vkey = target ^ bbmac(file.first(0x70), nullptr);
    }

    std::array<std::uint8_t, 0x30> descriptor;
    std::memcpy(descriptor.data(), file.data() + 0x30, descriptor.size());
    bbcipher(block_at(file, 0x10) ^ vkey, 1u, descriptor.data(), descriptor.size());
    Block data_key;
    std::memcpy(data_key.data(), descriptor.data(), 16);
    const std::uint32_t data_size = le32(descriptor.data() + 0x14);
    const std::uint32_t block_size = le32(descriptor.data() + 0x18);
    const std::uint32_t data_offset = le32(descriptor.data() + 0x1C);
    if (block_size == 0u || data_offset > file.size() || data_size > file.size() - data_offset) return false;

    // Blocks continue the counter of the previous one, so the data decrypts in one pass.
    plain.assign(file.begin() + data_offset, file.begin() + data_offset + data_size);
    bbcipher(data_key ^ vkey, 1u, plain.data(), plain.size());
    return true;
}

} // namespace pspweb::pgd
