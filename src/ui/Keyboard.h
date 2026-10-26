#pragma once

#include <memory>

class QKeyEvent;

// The keyboard typed with
namespace keyboard {

bool arabic();  // (the keyboard in use is an Arabic one)

// a digit typed with an Arabic keyboard, as Arabic's own (٠١٢...): Windows' Arabic keyboards type 0-9 - Word shows them
// as Arabic's as it lays them out. The key made so, for the box to type - none: the key as it is
std::unique_ptr<QKeyEvent> arabicDigit(const QKeyEvent *e);

}  // namespace keyboard
