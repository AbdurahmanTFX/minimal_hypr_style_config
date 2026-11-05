// layout_keys.hpp — горячие клавиши TUI в русской раскладке.
// Терминал присылает символ текущей раскладки, а программы сравнивают с латиницей,
// поэтому буквенные клавиши молчали. Переводим символ в ту же клавишу раскладки us.
// Применять только вне текстового ввода: имя заметки и пароль Wi-Fi менять нельзя.
#pragma once

#include <ftxui/component/event.hpp>

#include <cstddef>
#include <string>

inline ftxui::Event LatinKey(const ftxui::Event& event) {
  if (!event.is_character())
    return event;

  // Каждая кириллическая буква занимает в UTF-8 два байта.
  static const std::string kRu =
      "йцукенгшщзхъфывапролджэячсмитьбюЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ";
  static const std::string kEn =
      "qwertyuiop[]asdfghjkl;'zxcvbnm,.QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>";

  const std::string& ch = event.character();
  if (ch.size() != 2)
    return event;
  for (std::size_t i = 0; i < kEn.size(); ++i) {
    if (kRu.compare(i * 2, 2, ch) == 0)
      return ftxui::Event::Character(kEn[i]);
  }
  return event;
}
