#pragma once
#include <Arduino.h>
#include "SlotPlan.h"

// ============================================================================
// FramePlan
//   - Mengelola "role" tiap frame TDMA (DATA / HELLO / RDISC).
//   - Frame index dihitung dari unixTime / FRAME_LEN_SEC (sama seperti sekarang).
//   - Desain akhir: 1 frame = 1 tugas (DATA saja, atau HELLO saja, atau RDISC saja).
//
//   Step 1:
//     - API dan struktur sudah final.
//     - Implementasi sementara: SEMUA frame dianggap DATA_FRAME.
//       -> Tidak mengubah perilaku sistem sama sekali.
//
//   Step berikutnya (Step 2, Step 3):
//     - Kita tinggal ubah fungsi frameRoleForFrame() untuk
//       membagi super-cycle menjadi DATA / HELLO / RDISC.
// ============================================================================

namespace FramePlan {

using SlotPlan::FRAME_LEN_SEC;

// Jenis frame global
enum FrameRole : uint8_t {
  FRAME_DATA  = 0,  // frame hanya boleh dipakai untuk DATA (seperti sekarang)
  FRAME_HELLO = 1,  // frame khusus HELLO (link health)
  FRAME_RDISC = 2   // frame khusus ROUTE_DISCOVERY (RREQ/RREP flooding)
};

// --------------------------------------------------------------------------
// Desain final (konsep):
//
//   Kita punya "super-cycle" yang terdiri dari N frame, misalnya 10:
//     - frameIdx % 10 = 0..7  -> DATA_FRAME
//     - frameIdx % 10 = 8     -> HELLO_FRAME
//     - frameIdx % 10 = 9     -> RDISC_FRAME
//
//   Di Step 1 ini, kita BELUM mengaktifkan pola di atas.
//   Implementasi default: semua frame dianggap DATA_FRAME.
//   Nanti di Step 2/3, kita cukup modifikasi frameRoleForFrame().
// --------------------------------------------------------------------------

// Panjang satu frame dalam detik (sumber dari SlotPlan).
inline constexpr uint32_t frameLenSec() {
  return FRAME_LEN_SEC;
}

// ---- Konfigurasi pola super-cycle (masih belum dipakai di Step 1) ----
// Kamu bisa ganti ini nanti tanpa mengubah API di Node/Relay/Sink.
inline constexpr uint32_t SUPERFRAME_LEN = 10;  // jumlah frame dalam satu siklus

inline constexpr uint8_t DATA_FRAMES_PER_CYCLE = 8;   // index 0..7
inline constexpr uint8_t HELLO_FRAME_INDEX      = 8;  // index 8
inline constexpr uint8_t RDISC_FRAME_INDEX      = 9;  // index 9

// --------------------------------------------------------------------------
// frameRoleForFrame(frameIdx):
//   Input : index frame global (unixTime / FRAME_LEN_SEC)
//   Return: role frame (DATA / HELLO / RDISC)
//
//   Step 1: untuk sementara SEMUA frame -> FRAME_DATA.
//           -> Sistem tetap identik dengan versi sekarang.
//   Step 2/3: kita akan aktifkan pola super-cycle di sini.
// --------------------------------------------------------------------------
inline FrameRole frameRoleForFrame(uint32_t frameIdx) {
  // IMPLEMENTASI STEP 1:
  //   Semua frame dianggap DATA_FRAME.
  //   Tidak ada HELLO/RDISC yang aktif.
  (void)frameIdx; // supaya tidak warning unused
  return FRAME_DATA;

  // IMPLEMENTASI FINAL (NANTI, BUKAN SEKARANG):
  //
  // uint32_t pos = frameIdx % SUPERFRAME_LEN;
  // if (pos < DATA_FRAMES_PER_CYCLE) return FRAME_DATA;
  // if (pos == HELLO_FRAME_INDEX)    return FRAME_HELLO;
  // if (pos == RDISC_FRAME_INDEX)    return FRAME_RDISC;
  // return FRAME_DATA;  // fallback
}

// --------------------------------------------------------------------------
// frameRoleForUnix(unixTime, frameIdxOut):
//   Helper praktis:
//     - Hitung frameIdx dari unixTime.
//     - Sekaligus kembalikan frameRole.
// --------------------------------------------------------------------------
inline FrameRole frameRoleForUnix(uint32_t unixTime, uint32_t& frameIdxOut) {
  frameIdxOut = unixTime / frameLenSec();
  return frameRoleForFrame(frameIdxOut);
}

} // namespace FramePlan
