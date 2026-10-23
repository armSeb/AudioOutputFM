// stream_sniff.h - tell MP3 from AAC (ADTS) by looking at the first bytes of a stream.
//
// Web radios don't reliably say what they send (the URL may end in .mp3 for an AAC stream, and
// ESP8266Audio's ICY source doesn't expose Content-Type), so the sketch reads the first couple
// of KB, looks for audio frame headers, then replays those bytes to the decoder through
// PrefixSource so nothing is lost.
#pragma once
#include <Arduino.h>
#include "AudioFileSource.h"

// Serves a block of already-read bytes first, then the rest of the underlying source.
class PrefixSource : public AudioFileSource {
public:
    PrefixSource(AudioFileSource *src, const uint8_t *pre, uint32_t len) : src_(src), pre_(pre), len_(len) {}
    uint32_t read(void *data, uint32_t len) override { return get(data, len, false); }
    uint32_t readNonBlock(void *data, uint32_t len) override { return get(data, len, true); }
    bool close() override { return src_->close(); }
    bool isOpen() override { return src_->isOpen(); }
    uint32_t getSize() override { return src_->getSize(); }
    uint32_t getPos() override { return src_->getPos(); }
    bool loop() override { return src_->loop(); }

private:
    uint32_t get(void *data, uint32_t len, bool nb)
    {
        if (pos_ < len_) {
            uint32_t n = (len < len_ - pos_) ? len : len_ - pos_;
            memcpy(data, pre_ + pos_, n);
            pos_ += n;
            return n;
        }
        return nb ? src_->readNonBlock(data, len) : src_->read(data, len);
    }
    AudioFileSource *src_;
    const uint8_t   *pre_;
    uint32_t         len_, pos_ = 0;
};

enum StreamCodec { CODEC_MP3, CODEC_AAC };

// ADTS header: 12-bit sync, layer 00, frame length in bits 30..42. Requiring a second header
// exactly one frame later makes a false positive on MP3 data (or an ID3 tag) very unlikely.
static inline bool isAdts(const uint8_t *b) { return b[0] == 0xFF && (b[1] & 0xF6) == 0xF0; }

static inline StreamCodec sniffCodec(const uint8_t *b, int n)
{
    for (int i = 0; i + 7 <= n; i++) {
        if (!isAdts(b + i)) continue;
        int len = ((b[i + 3] & 0x03) << 11) | (b[i + 4] << 3) | (b[i + 5] >> 5);
        if (len < 7) continue;
        if (i + len + 2 > n) break;                     // can't confirm: assume MP3
        if (isAdts(b + i + len)) return CODEC_AAC;
    }
    return CODEC_MP3;
}
