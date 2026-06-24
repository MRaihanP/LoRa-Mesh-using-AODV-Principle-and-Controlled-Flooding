#pragma once
#include <Arduino.h>
#include <utility>
#include <stdint.h>

// =====================================================
//  Helper (C++11 SFINAE) untuk memanggil API callback / getter
//  tanpa bikin firmware gagal compile jika method tidak ada.
//  Diletakkan di header (.h) agar Arduino preprocessor tidak merusak template.
// =====================================================

// setTxCallback(cb)
template <typename Dev, typename F>
static auto trySetTxCallback(Dev& d, F f, int) -> decltype(d.setTxCallback(f), void()) {
  d.setTxCallback(f);
}
template <typename Dev, typename F>
static void trySetTxCallback(Dev&, F, ...) {}

// setAckCallback(cb)
template <typename Dev, typename F>
static auto trySetAckCallback(Dev& d, F f, int) -> decltype(d.setAckCallback(f), void()) {
  d.setAckCallback(f);
}
template <typename Dev, typename F>
static void trySetAckCallback(Dev&, F, ...) {}

// setForwardRxCallback(cb) (opsional)
template <typename Dev, typename F>
static auto trySetForwardRxCallback(Dev& d, F f, int) -> decltype(d.setForwardRxCallback(f), void()) {
  d.setForwardRxCallback(f);
}
template <typename Dev, typename F>
static void trySetForwardRxCallback(Dev&, F, ...) {}

// neighborCount() / getNeighborCount() (opsional)
template <typename Dev>
static auto tryNeighborCount1(Dev& d, int) -> decltype(d.neighborCount(), uint8_t()) {
  return (uint8_t)d.neighborCount();
}
template <typename Dev>
static uint8_t tryNeighborCount1(Dev&, ...) { return 0; }

template <typename Dev>
static auto tryNeighborCount2(Dev& d, int) -> decltype(d.getNeighborCount(), uint8_t()) {
  return (uint8_t)d.getNeighborCount();
}
template <typename Dev>
static uint8_t tryNeighborCount2(Dev&, ...) { return 0; }
