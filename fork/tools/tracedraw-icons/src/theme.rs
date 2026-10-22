// SPDX-License-Identifier: GPL-2.0-or-later
//! Light-theme colour tokens TraceDraw's icons.rs reads (TraceDraw LIGHT palette).
use egui::Color32;
#[derive(PartialEq, Eq, Clone, Copy)]
pub enum Theme { Light, HighContrast }
impl Theme { pub fn is_dark(&self) -> bool { false } }
pub fn theme() -> Theme { Theme::Light }
pub struct Tokens;
impl Tokens {
    pub const ACCENT: Color32 = Color32::from_rgb(0x00, 0x78, 0xD7);
    pub fn icon() -> Color32 { Color32::from_rgb(0x2A, 0x2A, 0x2A) }
    pub fn panel() -> Color32 { Color32::from_rgb(0xF4, 0xF4, 0xF4) }
}
