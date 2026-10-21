// Voice chat: push-to-talk (or always on) capture with WinMM, 16 kHz mono, 20 ms frames, IMA ADPCM
// (~68 kbit/s), sent unreliable on the voice channel. Every remote player gets its own waveOut stream
// with a small jitter buffer; the volume fades with distance down to a "radio" floor.
// WinMM runs without callbacks: buffers are polled once per game frame (8 x 20 ms of capture headroom).
// The microphone opens when push-to-talk is first held (at once only with "always on") and closes again after half a
// minute without it, so Windows shows it in use only around talking. /mute <nick> silences one player here.
#include "features/voice/Voice.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/voice/Adpcm.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

namespace {
constexpr int kRate = 16000, kFrame = 320;  // 20 ms
constexpr int kInBuffers = 8, kOutBuffers = 16;
constexpr size_t kMaxQueue = 12;  // 240 ms: older audio is dropped
constexpr size_t kStartQueue = 3;  // 60 ms of buffering before a stream starts

WAVEFORMATEX Format16k() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = kRate * 2;
    return f;
}

struct Stream {
    HWAVEOUT out = nullptr;
    WAVEHDR hdr[kOutBuffers] = {};
    int16_t buf[kOutBuffers][kFrame] = {};
    std::deque<std::vector<int16_t>> queue;
    bool playing = false;
    uint16_t lastSeq = 0;
    bool hasSeq = false;
    uint64_t lastHeardMs = 0;

    ~Stream() {
        if (!out) return;
        waveOutReset(out);
        for (WAVEHDR& h : hdr)
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out, &h, sizeof(h));
        waveOutClose(out);
    }
};

class Voice : public Module {
public:
    const char* Name() const override { return "Voice"; }

    void OnInstallHooks() override {
        Net().SetHandler(proto::G_VOICE, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
        Ini ini;  // test setups: a 440 Hz tone instead of the microphone (no device needed), transmitted all the time
        ini.Load(Cfg().iniPath);
        m_testTone = ini.GetBool("debug", "voice_test_tone", false);
    }

    void OnSessionStart() override {
        m_micWarned = m_silenceWarned = false;
        if (Cfg().voiceEnabled && Cfg().voiceAlwaysOn && !m_testTone) OpenMic();
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "mute" && cmd != "unmute") return false;
        auto& chat = ui::TheChat();
        if (args.empty()) {
            std::string names;
            for (const std::string& n : m_muted) names += (names.empty() ? "" : ", ") + n;
            if (names.empty()) chat.AddSystem(TR("Заглушить игрока: /mute <ник>, вернуть звук: /unmute <ник>.", "Mute a player: /mute <nick>, unmute: /unmute <nick>."));
            else chat.AddSystem(Format(TR("Заглушены: %s.", "Muted: %s."), names.c_str()));
            return true;
        }
        uint8_t id = Net().FindPeer(args[0]);
        std::string name = id != 255 ? Net().PeerName(id) : args[0];
        if (cmd == "mute") {
            if (id == 255) {
                chat.AddError(Format(TR("Нет игрока «%s».", "There is no player \"%s\"."), args[0].c_str()));
                return true;
            }
            m_muted.insert(ToLower(name));
            if (auto it = m_streams.find(id); it != m_streams.end()) it->second->queue.clear();
            chat.AddSystem(Format(TR("Голос %s заглушён (только у вас).", "%s is muted (for you only)."), name.c_str()));
        } else {
            m_muted.erase(ToLower(name));
            chat.AddSystem(Format(TR("Голос %s снова слышен.", "You hear %s again."), name.c_str()));
        }
        return true;
    }
    void OnSessionEnd() override {
        CloseMic();
        m_streams.clear();
    }
    void OnPeerLeave(uint8_t id) override { m_streams.erase(id); }

    void OnPostProcess() override {
        if (!Net().Online()) return;
        PumpCapture();
        if (m_transmitting) m_talkedMs = NowMs();
        else if (m_in && !Cfg().voiceAlwaysOn && NowMs() - m_talkedMs > 30000) {  // not in use: Windows stops showing it
            CloseMic();
            LOGI("Voice: microphone closed (not used for 30 s)");
        }
        for (auto& [id, s] : m_streams) PumpPlayback(*s);
        if (m_rxFrames && NowMs() - m_rxLogMs > 5000) {
            m_rxLogMs = NowMs();
            LOGI("Voice: %d frames received, rms %.0f", m_rxFrames, std::sqrt(m_rxEnergy / (m_rxFrames * (double)kFrame)));
            m_rxFrames = 0;
            m_rxEnergy = 0;
        }
    }

    void OnRender() override {
        if (!core::IsPlayerReady()) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), y = g.Height() * 0.62f;
        if (m_transmitting) {
            g.Text(ui::FONT_SMALL, 24 * s, y, TR("{FF5050}●{FFFFFF} Микрофон", "{FF5050}●{FFFFFF} Mic"), 0xFFFFFFFF, ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_SMALL);
        }
        for (auto& [id, stream] : m_streams)  // who is heard now, wherever they are (the name tag needs them in sight)
            if (Speaking(id)) {
                g.Text(ui::FONT_SMALL, 24 * s, y, "{FFDC50}((•)){FFFFFF} " + Net().PeerName(id), 0xFFFFFFFF, ui::TEXT_OUTLINE);
                y += g.LineHeight(ui::FONT_SMALL);
            }
    }

    bool Speaking(uint8_t id) const {
        auto it = m_streams.find(id);
        return it != m_streams.end() && NowMs() - it->second->lastHeardMs < 300;
    }
    bool Transmitting() const { return m_transmitting; }

    static Voice& Instance() {
        static Voice v;
        return v;
    }

private:
    bool OpenMic() {
        WAVEFORMATEX f = Format16k();
        UINT dev = Cfg().micDevice < 0 ? WAVE_MAPPER : (UINT)Cfg().micDevice;
        MMRESULT r = waveInOpen(&m_in, dev, &f, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            m_in = nullptr;
            LOGW("Voice: no microphone available (waveInOpen failed: %u)", r);
            return false;
        }
        for (int i = 0; i < kInBuffers; ++i) {
            m_inHdr[i] = {};
            m_inHdr[i].lpData = (LPSTR)m_inBuf[i];
            m_inHdr[i].dwBufferLength = sizeof(m_inBuf[i]);
            waveInPrepareHeader(m_in, &m_inHdr[i], sizeof(WAVEHDR));
            waveInAddBuffer(m_in, &m_inHdr[i], sizeof(WAVEHDR));
        }
        m_nextIn = 0;
        if ((r = waveInStart(m_in)) != MMSYSERR_NOERROR) {
            LOGW("Voice: the microphone does not start (waveInStart failed: %u)", r);
            CloseMic();
            return false;
        }
        LOGI("Voice: microphone open (push to talk: key %02X%s)", Cfg().voiceKey, Cfg().voiceAlwaysOn ? ", always on" : "");
        return true;
    }

    void CloseMic() {
        if (!m_in) return;
        waveInReset(m_in);
        for (WAVEHDR& h : m_inHdr)
            if (h.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(m_in, &h, sizeof(h));
        waveInClose(m_in);
        m_in = nullptr;
        m_transmitting = false;
    }

    bool PushToTalk() const {
        if (Cfg().voiceAlwaysOn || m_testTone) return true;
        // only while our window is active and nobody types in the chat
        return game::IsForeground() && !ui::TheChat().IsOpen() && (GetAsyncKeyState(Cfg().voiceKey) & 0x8000);
    }

    void PumpCapture() {
        m_transmitting = Cfg().voiceEnabled && PushToTalk();  // the indicator shows the key even without a microphone
        if (m_testTone) {
            uint64_t now = NowMs();
            if (now - m_toneMs > 200) m_toneMs = now;
            for (; now - m_toneMs >= 20; m_toneMs += 20) {  // a frame every 20 ms
                int16_t pcm[kFrame];
                for (int k = 0; k < kFrame; ++k, ++m_tonePhase) pcm[k] = (int16_t)(8000 * std::sin(m_tonePhase * 0.1727876f));
                Send(pcm);
            }
            LogSent();
            return;
        }
        if (!m_in) {
            // the microphone may be plugged in or allowed later: tried again while the key is held
            if (!m_transmitting || NowMs() - m_micTriedMs < 3000) return;
            m_micTriedMs = NowMs();
            if (OpenMic() || m_micWarned) return;
            m_micWarned = true;
            ui::TheChat().AddError(TR("Не удалось открыть микрофон. Проверьте, что он подключён, а в Windows разрешён доступ к микрофону для классических приложений.",
                                      "Could not open the microphone. Check that it is plugged in and that Windows lets desktop apps use the microphone."));
            return;
        }
        // in the order they were queued: by index, a wrap-around would swap 20 ms pieces
        for (int n = 0; n < kInBuffers; ++n) {
            WAVEHDR& h = m_inHdr[m_nextIn];
            if (!(h.dwFlags & WHDR_DONE)) break;
            if (m_transmitting && h.dwBytesRecorded == sizeof(m_inBuf[m_nextIn])) Send(m_inBuf[m_nextIn]);
            else m_enc = {};  // a new talk spurt starts from silence
            h.dwFlags &= ~WHDR_DONE;
            h.dwBytesRecorded = 0;
            waveInAddBuffer(m_in, &h, sizeof(WAVEHDR));
            m_nextIn = (m_nextIn + 1) % kInBuffers;
        }
        LogSent();
    }

    void Send(int16_t* pcm) {
        float gain = Cfg().micGain;
        for (int k = 0; k < kFrame; ++k) {
            if (gain != 1.0f) pcm[k] = (int16_t)std::clamp(pcm[k] * gain, -32768.0f, 32767.0f);
            m_txEnergy += (double)pcm[k] * pcm[k];
        }
        ++m_txFrames;
        std::vector<uint8_t> data = adpcm::Encode(pcm, kFrame, m_enc);
        Writer w(proto::G_VOICE);
        w.U16(m_seq++);
        w.Bytes(data.data(), data.size());
        Net().Send(proto::CH_VOICE, w);
    }

    // what the microphone gives: a privacy block in Windows records pure silence
    void LogSent() {
        if (!m_txFrames || NowMs() - m_txLogMs < 5000) return;
        m_txLogMs = NowMs();
        double rms = std::sqrt(m_txEnergy / (m_txFrames * (double)kFrame));
        LOGI("Voice: %d frames sent, rms %.0f", m_txFrames, rms);
        if (rms < 1.0 && m_txFrames >= 100 && !m_silenceWarned) {
            m_silenceWarned = true;
            ui::TheChat().AddError(TR("Микрофон передаёт тишину. Проверьте, что в Windows разрешён доступ к микрофону для классических приложений, а нужный микрофон выбран устройством записи по умолчанию.",
                                      "The microphone sends only silence. Check that Windows lets desktop apps use the microphone and that the right one is the default recording device."));
        }
        m_txFrames = 0;
        m_txEnergy = 0;
    }

    void Handle(uint8_t from, Reader& r) {
        uint16_t seq = r.U16();
        std::vector<uint8_t> data = r.Bytes(1024);
        std::vector<int16_t> pcm;
        if (!r.Ok() || from == Net().MyId() || !Cfg().voiceEnabled || m_muted.count(ToLower(Net().PeerName(from))) ||
            !adpcm::Decode(data.data(), data.size(), pcm) || pcm.size() != kFrame)
            return;
        auto& slot = m_streams[from];
        if (!slot) slot = std::make_unique<Stream>();
        Stream& s = *slot;
        if (s.hasSeq && (int16_t)(seq - s.lastSeq) <= 0) return;  // late or duplicate
        for (int16_t v : pcm) m_rxEnergy += (double)v * v;
        ++m_rxFrames;
        s.lastSeq = seq;
        s.hasSeq = true;
        s.lastHeardMs = NowMs();
        if (RemotePlayer* rp = Players::Instance().Get(from)) rp->heardMs = s.lastHeardMs;  // its name tag
        float vol = Cfg().voiceVolume * Proximity(from);
        for (int16_t& v : pcm) v = (int16_t)std::clamp(v * vol, -32768.0f, 32767.0f);
        s.queue.push_back(std::move(pcm));
        while (s.queue.size() > kMaxQueue) s.queue.pop_front();
    }

    // full volume up to 15 m, then down to a radio-like floor at 80 m (co-op partners keep talking)
    static float Proximity(uint8_t id) {
        if (!Cfg().voiceProximity || !core::IsPlayerReady()) return 1.0f;
        const RemotePlayer* rp = Players::Instance().Get(id);
        if (!rp || !rp->hasState) return 1.0f;
        float d = (rp->state.pos - game::FindPlayerPed(0)->GetPosition()).Length();
        return std::clamp(1.0f - (d - 15.0f) / 65.0f, 0.35f, 1.0f);
    }

    void PumpPlayback(Stream& s) {
        if (!s.out) {
            if (s.queue.empty()) return;
            WAVEFORMATEX f = Format16k();
            UINT dev = Cfg().speakerDevice < 0 ? WAVE_MAPPER : (UINT)Cfg().speakerDevice;
            if (waveOutOpen(&s.out, dev, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                s.out = nullptr;
                s.queue.clear();
                return;
            }
            for (WAVEHDR& h : s.hdr) h.dwFlags = WHDR_DONE;  // all free
        }
        if (!s.playing && s.queue.size() < kStartQueue) return;
        bool any = false;
        for (int i = 0; i < kOutBuffers && !s.queue.empty(); ++i) {
            WAVEHDR& h = s.hdr[i];
            if (!(h.dwFlags & WHDR_DONE)) {
                any = true;
                continue;
            }
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(s.out, &h, sizeof(h));
            memcpy(s.buf[i], s.queue.front().data(), sizeof(s.buf[i]));
            s.queue.pop_front();
            h = {};
            h.lpData = (LPSTR)s.buf[i];
            h.dwBufferLength = sizeof(s.buf[i]);
            waveOutPrepareHeader(s.out, &h, sizeof(h));
            waveOutWrite(s.out, &h, sizeof(h));
            any = true;
        }
        s.playing = any || !s.queue.empty();  // re-buffer after a gap
    }

    HWAVEIN m_in = nullptr;
    int m_nextIn = 0;  // the capture buffer the device fills first
    uint64_t m_micTriedMs = 0, m_talkedMs = 0;
    std::set<std::string> m_muted;  // nicknames (lower case): a muted player stays so when it joins again
    bool m_micWarned = false, m_silenceWarned = false;
    WAVEHDR m_inHdr[kInBuffers] = {};
    int16_t m_inBuf[kInBuffers][kFrame] = {};
    adpcm::State m_enc;
    uint16_t m_seq = 0;
    bool m_transmitting = false;
    std::map<uint8_t, std::unique_ptr<Stream>> m_streams;
    bool m_testTone = false;
    unsigned m_tonePhase = 0;
    uint64_t m_toneMs = 0;
    int m_rxFrames = 0, m_txFrames = 0;
    double m_rxEnergy = 0, m_txEnergy = 0;
    uint64_t m_rxLogMs = 0, m_txLogMs = 0;
};

struct Reg {
    Reg() { core::RegisterModule(&Voice::Instance()); }
} g_reg;
}  // namespace

bool VoiceIsSpeaking(uint8_t id) { return Voice::Instance().Speaking(id); }
bool VoiceIsTransmitting() { return Voice::Instance().Transmitting(); }

}  // namespace coop
