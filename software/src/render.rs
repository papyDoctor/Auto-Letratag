//! Rendering of a text into columns of dots with a TTF font

use std::path::Path;

use anyhow::{anyhow, Context, Result};
use fontdue::{Font, FontSettings};

/// The scale is chosen so that the characters of the text use the full height of the head:
/// an uppercase text fills the 30 dots, a text with descenders (g, p, q, y) is a little
/// smaller. The digits always count, so all the labels of a series
/// (which differ only by their number) have the same size.
const ALWAYS: &str = "0123456789";

/// Line separator in the text: `BOX-{n:03}|Kitchen` gives two lines
pub const SEPARATOR: char = '|';

/// Blank dots between two lines of text
const LINE_GAP: u32 = 2;

/// A pixel is black above this coverage (0-255)
const THRESHOLD: u8 = 128;

pub struct Renderer {
    font: Font,
    height: u32,
    /// Character width relative to the original font (1.0 = normal proportions)
    stretch: f32,
}

impl Renderer {
    /// Loads a font and scales it to fit on `height` dots
    pub fn load(path: &Path, height: u32, stretch: f32) -> Result<Self> {
        let bytes =
            std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let font = Font::from_bytes(bytes, FontSettings::default())
            .map_err(|e| anyhow!("font {}: {e}", path.display()))?;

        Ok(Self { font, height, stretch })
    }

    /// Extreme black rows (above the baseline: negative) of the characters of
    /// `text` and of the digits, rendered at size `px` with the same threshold as printing
    fn extent(&self, text: &str, px: f32) -> Option<(i32, i32)> {
        let mut extent: Option<(i32, i32)> = None;
        for c in text.chars().chain(ALWAYS.chars()) {
            let (m, pixels) = self.font.rasterize(c, px);
            let top = -(m.ymin + m.height as i32);
            for j in 0..m.height {
                if pixels[j * m.width..(j + 1) * m.width].iter().any(|&p| p >= THRESHOLD) {
                    let y = top + j as i32;
                    extent = Some(extent.map_or((y, y), |(t, b)| (t.min(y), b.max(y))));
                }
            }
        }
        extent
    }

    /// Largest size in pixels for which the characters of `text` and the digits
    /// fit on the height, and baseline (in dots from the top) that places the
    /// highest black row on the first dot
    fn scale(&self, text: &str, height: u32) -> (f32, f32) {
        let mut px = 2.0 * height as f32;
        while px > 1.0 {
            if let Some((t, b)) = self.extent(text, px) {
                if b - t < height as i32 {
                    return (px, -t as f32);
                }
            }
            px *= 0.99;
        }
        (px, 0.0)
    }

    /// Columns from left to right; bit r of a column is row r counting from the top.
    /// The text can have several lines separated by `|`: they share the height
    /// (2 blank dots between them), are left-aligned and drawn at the same size.
    pub fn draw(&self, text: &str) -> Vec<u32> {
        let lines: Vec<&str> = text.split(SEPARATOR).collect();
        let n = lines.len() as u32;
        let h = self.height.saturating_sub(LINE_GAP * (n - 1)) / n;
        if n == 1 || h < 5 {
            return self.draw_line(&text.replace(SEPARATOR, " "), text, self.height);
        }
        // all lines at the same scale: the one that makes all their characters fit
        let reference: String = lines.concat();
        let margin = (self.height - n * h - LINE_GAP * (n - 1)) / 2;
        let mut columns: Vec<u32> = Vec::new();
        for (k, line) in lines.iter().enumerate() {
            let offset = margin + k as u32 * (h + LINE_GAP);
            let rendered = self.draw_line(line, &reference, h);
            if columns.len() < rendered.len() {
                columns.resize(rendered.len(), 0);
            }
            for (x, c) in rendered.into_iter().enumerate() {
                columns[x] |= c << offset;
            }
        }
        columns
    }

    /// One line of text on `height` rows, at the scale that makes the characters of
    /// `reference` fit. To widen the characters, the text is rendered taller
    /// (height x stretch) then brought back to `height` by keeping only some of the rows.
    fn draw_line(&self, text: &str, reference: &str, height: u32) -> Vec<u32> {
        let tall = ((height as f32 * self.stretch).round() as u32).clamp(height, 64);
        let big = self.draw_on(text, reference, tall);
        big
            .iter()
            .map(|&c| {
                (0..height).fold(0u32, |output, y| {
                    let source = (((y as f32 + 0.5) * tall as f32 / height as f32) as u32).min(tall - 1);
                    output | (((c >> source) & 1) as u32) << y
                })
            })
            .collect()
    }

    /// Rendering on `height` rows (64 at most)
    fn draw_on(&self, text: &str, reference: &str, height: u32) -> Vec<u64> {
        let (px, baseline) = self.scale(reference, height);
        let mut columns: Vec<u64> = Vec::new();
        let mut pen = 0f32;
        let mut previous = None;

        for c in text.chars() {
            if let Some(p) = previous {
                pen += self.font.horizontal_kern(p, c, px).unwrap_or(0.0);
            }
            let (m, pixels) = self.font.rasterize(c, px);
            let left = (pen + m.xmin as f32).round() as i32;
            let top = (baseline - (m.ymin as f32 + m.height as f32)).round() as i32;

            for j in 0..m.height {
                for i in 0..m.width {
                    let (x, y) = (left + i as i32, top + j as i32);
                    if pixels[j * m.width + i] < THRESHOLD || x < 0 || y < 0 || y >= height as i32
                    {
                        continue;
                    }
                    let x = x as usize;
                    if columns.len() <= x {
                        columns.resize(x + 1, 0);
                    }
                    columns[x] |= 1u64 << y;
                }
            }
            pen += m.advance_width;
            previous = Some(c);
        }

        let width = pen.ceil().max(0.0) as usize;
        if columns.len() < width {
            columns.resize(width, 0);
        }
        columns
    }
}

#[cfg(test)]
mod tests {
    use super::Renderer;
    use std::path::Path;

    #[test]
    fn fits_in_height() {
        let path = Path::new(crate::DEFAULT_FONT);
        if !path.exists() {
            return;
        }
        let renderer = Renderer::load(path, 29, 1.0).unwrap();
        let columns = renderer.draw("BOX-007 gjpqy");
        assert!(!columns.is_empty());
        assert!(columns.iter().all(|&c| c < 1 << 29));
        // the text does reach the top and bottom of the area
        let all = columns.iter().fold(0, |a, &c| a | c);
        assert!(all & 0b11 != 0 && all >> 27 != 0);
    }

    #[test]
    fn uppercase_uses_full_height() {
        let path = Path::new(crate::DEFAULT_FONT);
        if !path.exists() {
            return;
        }
        for stretch in [1.0, 1.08] {
            let renderer = Renderer::load(path, 30, stretch).unwrap();
            let all = renderer.draw("BOX-001").iter().fold(0, |a, &c| a | c);
            // first and last dot used, nothing beyond
            assert!(all & 1 != 0, "top not reached: {all:030b}");
            assert!(all & (1 << 29) != 0, "bottom not reached: {all:030b}");
            assert!(all < 1 << 30);
        }
    }

    #[test]
    fn two_lines() {
        let path = Path::new(crate::DEFAULT_FONT);
        if !path.exists() {
            return;
        }
        let renderer = Renderer::load(path, 30, 1.08).unwrap();
        let columns = renderer.draw("BOX-001|KITCHEN");
        let all = columns.iter().fold(0, |a, &c| a | c);
        // 14 + 2 + 14 dots: both lines touch the edges, the line gap stays blank
        assert!(all & 1 != 0 && all & (1 << 29) != 0, "{all:030b}");
        assert_eq!(all & (0b11 << 14), 0, "line gap: {all:030b}");
        assert!(all < 1 << 30);
        // shorter than a single line with the same text
        let single = renderer.draw("BOX-001 KITCHEN").len();
        assert!(columns.len() < single);
    }

    #[test]
    fn stretch_widens() {
        let path = Path::new(crate::DEFAULT_FONT);
        if !path.exists() {
            return;
        }
        let normal = Renderer::load(path, 30, 1.0).unwrap().draw("BOX-001").len() as f32;
        let wide = Renderer::load(path, 30, 1.08).unwrap().draw("BOX-001").len() as f32;
        assert!((wide / normal - 1.08).abs() < 0.04, "{normal} -> {wide}");
    }
}
