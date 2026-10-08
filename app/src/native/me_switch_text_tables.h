// Mass Effect - "Nintendo Switch" wording for the game's talk-table (TLK) strings.
//
// Shared by app/src/native/me_switch_text.cpp (English edition) and
// editions/ru/overlay/app/src/native/me_switch_text.cpp (Russian edition). Only the tables live here; the
// hook (and the guest addresses) are in the .cpp files. See docs/switch-branding.md.
//
// The string ids (strrefs) are the same in both editions; the texts are not. Every id was taken from the
// game's own talk tables (GlobalTlk.xxx and the per-map *_tlk exports), read-only, on the PC.
//
// Two kinds of entries:
//   kWhole*    the whole string is replaced (platform messages: storage, users, store, HOME button).
//   kPhrase*   (from, to) pairs applied only to the listed ids (controller wording in tutorials).
// Button letters (A/B/X/Y) are not touched here: with the default face-button mapping (input_xbox_layout =
// false) the Switch button with the same letter does the same thing. With input_xbox_layout = true the hook
// swaps the letters (see SwapFaceLetters in me_switch_text.cpp).
//
// Russian notes: the Russian GlobalTlk writes the Latin "x" instead of the Cyrillic "х" in menu strings
// (for example "необxодимые"); the replacements below do the same so they render with the same glyphs.
// The Russian tables also never use "ё", "Ё" or "Ъ".

#pragma once

#include <cstdint>

namespace me::switch_text {

struct WholeEntry {
  int32_t id;
  const char* text;  // UTF-8
};

struct PhraseEntry {
  const char* from;  // UTF-8, exact (case-sensitive) substring
  const char* to;
};

// ---------------------------------------------------------------------------------------------------------
// English
// ---------------------------------------------------------------------------------------------------------
inline constexpr WholeEntry kWholeEn[] = {
    {153006, "The user has changed. Mass Effect will restart now."},
    {153062, "REMOVED: The storage device containing critical game files. Returning to Main Menu."},
    {153723, "WARNING: Downloadable Content is installed. You may experience slight performance problems."},
    {153801, "Initializing Downloadable Content. Please wait."},
    {154280, "Selected Save Game could not be loaded. Downloadable Content is missing from this system. "
             "Module(s) in question: "},
    {163473, "Nintendo Switch Controller"},
    {167959, "User Profile"},
    {169079, "Closes the options menu and saves your selections to your user profile."},
    {169456, "The specified device could not be accessed. Please ensure that a valid storage device is "
             "available on your Nintendo Switch system before continuing. You will be prompted to select a new "
             "device."},
    {169457, "No user is selected. You cannot access save data without a user. Please select a user on the "
             "Nintendo Switch system and return to the Main Menu."},
    {171439, "Save Game Failed: a user must be selected."},
    {173617, "New downloadable content available."},
    {174310, "No user is selected. To select a user, press the HOME Button. If you continue, you will not be "
             "able to save your progress."},
    {174312, "Gameplay options are only stored in Save Games, not in user profiles. Save your game when you "
             "exit the OPTIONS menu to save your gameplay changes."},
    {174444, "The specified device could not be accessed. Please ensure that a valid storage device is "
             "available on your Nintendo Switch system."},
    // Button names in the (PC-era) key binding list. Unused on the console as far as we know; harmless.
    {174637, "Press Left Stick"},
    {174638, "Press Right Stick"},
    {174719, "Left Stick X Axis"},
    {174720, "ZL Button"},
    {174803, "Right Stick X Axis"},
    {174804, "Right Stick Y Axis"},
    {174805, "Right Stick Y Axis"},
    {174806, "ZR Button"},
    {174829, "+ Button"},
};

// Ids whose text gets kPhraseEn applied (tutorial and help texts that name controller parts).
inline constexpr int32_t kPhraseIdsEn[] = {
    165144, 165145, 165146, 165147, 165148, 165149, 165150, 165151, 165152, 165153, 165154,
    165155, 165156, 165157, 165158, 165159, 165160, 165161, 165162, 165163, 165164, 165165, 165166, 165167,
    165169, 165170, 165171, 165172, 165173, 165174, 165175, 165176, 165177, 165178, 165179, 165180, 165181,
    165182, 165183, 165184, 165185, 165186, 165537, 165538, 165548, 168205, 168882, 168884,
    168885, 168887, 168888, 168889, 168890, 168893, 168896, 168899,
    169943, 169944, 169960, 169964, 172588, 172596, 172597, 172598, 172636, 172637, 172785, 172793, 172794,
    173195, 173198, 173199, 173200, 173205, 173216, 173217, 173218, 173219, 173220, 173221, 173222,
    173223, 173224, 173256, 173257,
};

// Ids whose face-button letters are swapped (A<->B, X<->Y) when input_xbox_layout = true, so the text names
// the Switch button the player actually presses. Only letters next to a "press"-like word are swapped.
inline constexpr int32_t kLetterIdsEn[] = {
    156265, 157774, 159946, 159994, 168881, 168883, 168885, 168886, 168890, 168891, 168892, 168895, 168957,
    169964, 172597, 172598, 172796, 173188, 173189, 173202, 173247, 173248, 173249, 173289, 173291, 173292,
    173293, 174362, 174596, 174626, 174857, 174859,
};
inline constexpr const char* kLetterBeforeEn[] = {"Press", "press", "Pressing", "pressing", "Hold", "hold",
                                                   "Tap", "tap", "and", "or"};
inline constexpr const char* kLetterAfterEn[] = {"Button", "button", "to", "draws", "holsters", "fires",
                                                  "returns"};

// Applied in order. Longer phrases first so "Left Bumpers" is not split by "Left Bumper".
inline constexpr PhraseEntry kPhraseEn[] = {
    {"Left and Right Bumpers", "L and R Buttons"},
    {"Left Bumper", "L Button"},
    {"Right Bumper", "R Button"},
    {"Left Trigger", "ZL Button"},
    {"Right Trigger", "ZR Button"},
    {"While holding RB", "While holding R"},
    {"Release RB", "Release R"},
    {"use RS to aim", "use the Right Stick to aim"},
    {"and LS to highlight", "and the Left Stick to highlight"},
    {"Press START", "Press +"},
    {"Press BACK", "Press -"},
    {"press BACK", "press -"},
    {"D-pad", "directional buttons"},
    // Achievements: there is no Gamerscore or gamer picture on the Switch; the in-game rewards stay.
    {" and Gamer Picture", ""},
    {"\nUnlock Gamer Picture", ""},
    {" Gamerscore", " points"},
};

// ---------------------------------------------------------------------------------------------------------
// Russian ("x" for "х" as in the original menu strings)
// ---------------------------------------------------------------------------------------------------------
inline constexpr WholeEntry kWholeRu[] = {
    {153006, "Пользователь изменен. Игра будет перезапущена."},
    {153062, "ОШИБКА: Извлечен носитель данныx, содержащий файлы, необxодимые для игры. Выxод в главное меню."},
    {153723, "ВНИМАНИЕ: Установлены загруженные файлы. Возможно небольшое уxудшение производительности."},
    {153801, "Запуск загруженныx файлов. Подождите."},
    {154280, "Невозможно загрузить соxраненную игру. Отсутствуют загруженные файлы. Ошибка модулей: "},
    {163473, "Контроллер Nintendo Switch"},
    {169456, "Доступ к выбранному устройству невозможен. Убедитесь, что в консоли Nintendo Switch есть "
             "исправный носитель данныx. Вам будет предложено выбрать другое устройство."},
    {169457, "Пользователь не выбран. Доступ к соxранениям без пользователя невозможен. Выберите пользователя "
             "на консоли Nintendo Switch и вернитесь в главное меню."},
    {171439, "Ошибка соxранения: пользователь не выбран."},
    {173617, "Доступны новые файлы для загрузки."},
    {174310, "Пользователь не выбран. Чтобы выбрать пользователя, нажмите кнопку HOME, в противном случае вы "
             "не сможете соxранять игру."},
    {174444, "Указанное устройство недоступно. Убедитесь, что в консоли Nintendo Switch есть исправное "
             "устройство xранения."},
    {174720, "Кнопка ZL"},
    {174806, "Кнопка ZR"},
    {174829, "Кнопка +"},
};

inline constexpr int32_t kPhraseIdsRu[] = {
    165150, 165151, 168205, 168887, 168888, 168889, 168890, 168896, 168899, 169960, 169964, 172596, 172597,
    172598, 172636, 172785, 172793, 172794, 173195, 173198, 173199, 173200, 173205, 173256, 173257,
};

inline constexpr int32_t kLetterIdsRu[] = {
    156265, 157774, 159994, 168881, 168883, 168884, 168885, 168886, 168890, 168891, 168892, 168895, 168957,
    169964, 172597, 172598, 172796, 173188, 173189, 173202, 173247, 173248, 173249, 173282, 173289, 173291,
    173292, 173293, 174362, 174596, 174626, 174857, 174859,
};
inline constexpr const char* kLetterBeforeRu[] = {"Нажмите", "нажмите", "Нажми", "нажми", "клавишу", "клавиши",
                                                   "Кнопка", "кнопка", "кнопку", "или", "и", "Нажав", "нажав",
                                                   "Нажатием", "нажатием"};
inline constexpr const char* kLetterAfterRu[] = {""};  // none: Russian puts the verb before the letter ("" never matches)

inline constexpr PhraseEntry kPhraseRu[] = {
    {"на правую и левую верxние кнопки", "на кнопки R и L"},
    {"левую верxнюю кнопку", "кнопку L"},
    {"левая верxняя кнопка", "кнопка L"},
    {"правую верxнюю кнопку", "кнопку R"},
    {"правая верxняя кнопка", "кнопка R"},
    {"(левый курок)", "(кнопка ZL)"},
    {"(правый курок)", "(кнопка ZR)"},
    {"левый курок", "кнопку ZL"},
    {"правого курка", "кнопки ZR"},
    {"правый курок", "кнопку ZR"},
    {"Нажми START.", "Нажми +"},
    {"Нажмите НАЗАД", "Нажмите -"},
    {"клавишу BACK", "кнопку -"},
    {" и аватара", ""},
    {"\nДоступна аватара", ""},
};

}  // namespace me::switch_text
