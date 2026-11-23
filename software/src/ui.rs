//! Display in the terminal

use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Clear, Gauge, List, ListState, Paragraph};
use ratatui::Frame;

use crate::{App, State, FIELDS};

pub fn draw(f: &mut Frame, app: &App) {
    let preview_rows = app.head.dots.div_ceil(4) as u16;
    let [title, form, preview, progress, log, help] = Layout::vertical([
        Constraint::Length(1),
        Constraint::Length(FIELDS.len() as u16 + 2),
        Constraint::Length(preview_rows + 3),
        Constraint::Length(3),
        Constraint::Min(3),
        Constraint::Length(2),
    ])
    .areas(f.area());

    f.render_widget(
        Paragraph::new(format!(
            " Letratag · {} · {} dots of {} µm · {}{}",
            app.printer.name,
            app.head.dots,
            app.head.pitch_um,
            if app.continuous { "continuous" } else { "stop at each line" },
            if app.mirror { " · mirrored" } else { "" }
        ))
        .style(Style::new().add_modifier(Modifier::BOLD)),
        title,
    );

    let editing = matches!(app.state, State::Editing);
    let fields: Vec<Line> = FIELDS
        .iter()
        .zip(&app.fields)
        .enumerate()
        .map(|(i, (name, value))| {
            let active = editing && i == app.focus;
            let mut line = vec![Span::raw(format!("{name:>10}: "))];
            match (active, app.selection) {
                // selected value: highlighted, the next keystroke replaces it
                (true, true) => {
                    line.push(Span::styled(value.as_str(), Style::new().fg(Color::Black).bg(Color::Cyan)))
                }
                (true, false) => {
                    line.push(Span::raw(value.as_str()));
                    line.push(Span::styled("▏", Style::new().fg(Color::Cyan)));
                }
                (false, _) => line.push(Span::raw(value.as_str())),
            }
            Line::from(line)
        })
        .collect();
    f.render_widget(Paragraph::new(fields).block(Block::bordered().title(" Series ")), form);

    let n = app.current_number();
    let text = app.text(n);
    let columns = app.renderer.draw(&text);
    let length_mm = columns.len() as f32 * app.head.pitch_um as f32 / 1000.0;
    let mut lines = braille(&columns, app.head.dots);
    lines.push(Line::styled(
        format!("{text}  ·  {} lines, text {length_mm:.1} mm long", columns.len()),
        Style::new().fg(Color::DarkGray),
    ));
    f.render_widget(
        Paragraph::new(lines).block(Block::bordered().title(format!(" Preview #{n} "))),
        preview,
    );

    let series = app
        .series_size()
        .map(|(rank, total)| format!("label {rank}/{total}"))
        .unwrap_or_default();
    let (ratio, label) = match &app.state {
        State::Editing => (0.0, "Ready".to_string()),
        State::Sending { .. } => (0.0, format!("Sending · {series}")),
        State::Printing { done, total, .. } => (
            *done as f64 / *total as f64,
            format!("Printing · {series} · {done}/{total} lines"),
        ),
        State::AwaitingCut { .. } => (1.0, format!("Cut the label ({series}), then press Enter")),
        State::Flushing { .. } => (1.0, "Feeding out and cutting the last label".to_string()),
    };
    f.render_widget(
        Gauge::default()
            .block(Block::bordered())
            .gauge_style(Style::new().fg(Color::Green))
            .ratio(ratio.clamp(0.0, 1.0))
            .label(label),
        progress,
    );

    let visible = log.height.saturating_sub(2) as usize;
    let messages: Vec<Line> = app
        .log
        .iter()
        .skip(app.log.len().saturating_sub(visible))
        .map(|m| {
            let style = if m.starts_with("Error") { Style::new().fg(Color::Red) } else { Style::new() };
            Line::styled(m.as_str(), style)
        })
        .collect();
    f.render_widget(Paragraph::new(messages).block(Block::bordered().title(" Log ")), log);

    let keys = match app.state {
        State::Editing if app.picker.is_some() => {
            "↑ ↓ select · Enter use this template · Delete remove from the list · Esc close"
        }
        State::Editing => {
            "Tab next field · Ctrl-N new line · Ctrl-S save template · Ctrl-O saved templates\n\
             Ctrl-P print series · Ctrl-A continuous/line by line · Ctrl-R mirror · Esc quit"
        }
        State::AwaitingCut { .. } => "Enter next label · Esc stop the series",
        _ => "Esc stop after this label · Ctrl-C quit",
    };
    f.render_widget(Paragraph::new(keys).style(Style::new().fg(Color::DarkGray)), help);

    if let Some(index) = app.picker {
        draw_picker(f, app, index);
    }
}

/// List of saved templates, on top of the rest
fn draw_picker(f: &mut Frame, app: &App, index: usize) {
    let screen = f.area();
    let width = app.registry.templates.iter().map(|t| t.chars().count()).max().unwrap_or(0) as u16 + 6;
    let width = width.clamp(40, screen.width.saturating_sub(4));
    let height = (app.registry.templates.len() as u16 + 2).min(screen.height.saturating_sub(4));
    let area = Rect {
        x: screen.x + (screen.width.saturating_sub(width)) / 2,
        y: screen.y + (screen.height.saturating_sub(height)) / 2,
        width,
        height,
    };
    let list = List::new(app.registry.templates.iter().map(String::as_str))
        .block(Block::bordered().title(" Saved templates "))
        .highlight_style(Style::new().fg(Color::Black).bg(Color::Cyan))
        .highlight_symbol("› ");
    let mut state = ListState::default().with_selected(Some(index));
    f.render_widget(Clear, area);
    f.render_stateful_widget(list, area, &mut state);
}

/// Braille preview: each character covers 2 columns x 4 rows of dots
fn braille(columns: &[u32], height: u32) -> Vec<Line<'static>> {
    // braille character bit for row r (0-3) of the left then right column
    const LEFT: [u32; 4] = [0x01, 0x02, 0x04, 0x40];
    const RIGHT: [u32; 4] = [0x08, 0x10, 0x20, 0x80];
    let dot = |x: usize, y: u32| y < height && columns.get(x).is_some_and(|c| c >> y & 1 == 1);

    (0..height.div_ceil(4))
        .map(|ry| {
            let row: String = (0..columns.len().div_ceil(2))
                .map(|cx| {
                    let mut code = 0;
                    for r in 0..4 {
                        let y = ry * 4 + r as u32;
                        if dot(cx * 2, y) {
                            code |= LEFT[r];
                        }
                        if dot(cx * 2 + 1, y) {
                            code |= RIGHT[r];
                        }
                    }
                    char::from_u32(0x2800 + code).unwrap_or(' ')
                })
                .collect();
            Line::raw(row)
        })
        .collect()
}
