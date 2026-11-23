//! Letratag: series printing of numbered labels on the modified LT-100H

mod printer;
mod registry;
mod render;
mod template;
mod ui;

use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use ratatui::DefaultTerminal;
use ratatui::crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};

use printer::{Head, Printer};
use registry::Registry;
use render::Renderer;

pub const DEFAULT_FONT: &str = "/System/Library/Fonts/Supplemental/Arial Bold.ttf";
const DEFAULT_HEAT_US: u32 = 5000;
/// Same ceiling as the heat limit in the firmware, which would otherwise silently clamp the value
const MAX_HEAT_US: i64 = 20000;

const USAGE: &str = "\
usage: letratag [--port PORT] [--font FILE.ttf] [--width FACTOR] [--templates FILE] [--mirror]
  --port       serial port (default: first Letratag board found)
  --font       TrueType font (default: Arial Bold)
  --width      character width, 1 to 2 (default 1.08: same length as an original Dymo label)
  --templates  saved templates file (default: ~/.config/letratag/templates.txt)
  --mirror     print columns right to left (if the text comes out mirrored)";

pub const FIELDS: [&str; 4] = ["Template", "Start", "End", "Heat (µs)"];
const TEMPLATE: usize = 0;
const START: usize = 1;
const END: usize = 2;
const HEAT: usize = 3;

pub enum State {
    Editing,
    /// IMG sent, waiting for READY to send the lines
    Sending {
        n: i64,
        lines: Vec<u32>,
    },
    Printing {
        n: i64,
        done: u32,
        total: u32,
    },
    /// Label fed out, waiting for the cut by hand (firmware without automatic cutter)
    AwaitingCut {
        n: i64,
    },
    /// Series stopped midway: FLUSH sent, the firmware feeds out and cuts the last label
    Flushing {
        n: i64,
    },
}

pub struct App {
    pub fields: [String; 4],
    pub focus: usize,
    /// Value of the active field selected: the next keystroke replaces it
    pub selection: bool,
    pub renderer: Renderer,
    pub head: Head,
    pub printer: Printer,
    rx: Receiver<printer::Event>,
    pub state: State,
    pub mirror: bool,
    /// Continuous printing (motor never stopped) or line by line
    pub continuous: bool,
    pub stop_requested: bool,
    pub registry: Registry,
    /// List of saved templates open (Ctrl-O), with the selected row
    pub picker: Option<usize>,
    /// k sent with the last IMG: 0 = chained label, its cut is still to be done
    k_sent: u8,
    pub log: Vec<String>,
    label_start: Instant,
    quit: bool,
}

impl App {
    fn number(&self, field: usize) -> Result<i64> {
        self.fields[field]
            .trim()
            .parse()
            .with_context(|| format!("{}: invalid number", FIELDS[field]))
    }

    /// Number of the label in progress, or of the start of the series when idle
    pub fn current_number(&self) -> i64 {
        match self.state {
            State::Sending { n, .. }
            | State::Printing { n, .. }
            | State::AwaitingCut { n }
            | State::Flushing { n } => n,
            State::Editing => self.number(START).unwrap_or(1),
        }
    }

    pub fn series_size(&self) -> Option<(i64, i64)> {
        let (start, end) = (self.number(START).ok()?, self.number(END).ok()?);
        (end >= start).then_some((self.current_number() - start + 1, end - start + 1))
    }

    pub fn text(&self, n: i64) -> String {
        template::format(&self.fields[TEMPLATE], n)
    }

    fn note(&mut self, message: impl Into<String>) {
        self.log.push(message.into());
        if self.log.len() > 100 {
            self.log.remove(0);
        }
    }

    fn start_series(&mut self) -> Result<()> {
        let (start, end) = (self.number(START)?, self.number(END)?);
        if end < start {
            bail!("the end ({end}) is before the start ({start})");
        }
        let heat = self.number(HEAT)?;
        if !(1..=MAX_HEAT_US).contains(&heat) {
            bail!("heat must be 1 to {MAX_HEAT_US} µs");
        }
        self.stop_requested = false;
        self.launch(start)
    }

    fn launch(&mut self, n: i64) -> Result<()> {
        let text = self.text(n);
        let shift = self.head.first - 1;
        let mut lines: Vec<u32> = self
            .renderer
            .draw(&text)
            .into_iter()
            .map(|c| c << shift)
            .collect();
        if self.mirror {
            lines.reverse();
        }
        if lines.is_empty() {
            bail!("label {n} is empty");
        }
        if lines.len() > self.head.max_lines as usize {
            bail!(
                "label {n} is too long ({} lines, maximum {})",
                lines.len(),
                self.head.max_lines
            );
        }
        let heat = self.number(HEAT)?;
        let mut command = format!("IMG {} {heat} {}", lines.len(), u8::from(self.continuous));
        if self.head.cut >= 2 {
            // k = 0: another label follows, the firmware will cut this one during the next
            self.k_sent = u8::from(n >= self.number(END)? || self.stop_requested);
            command += &format!(" {}", self.k_sent);
        }
        self.printer.send(&command)?;
        self.note(format!("Label {n}: {text}"));
        self.label_start = Instant::now();
        self.state = State::Sending { n, lines };
        Ok(())
    }

    fn on_line(&mut self, line: &str) -> Result<()> {
        if line.starts_with("ERR") {
            self.state = State::Editing;
            bail!("firmware: {line}");
        }
        if line.starts_with("SPEED") {
            self.note(format!("  {line}"));
        }
        match &mut self.state {
            State::Sending { .. } if line.starts_with("READY") => {
                let State::Sending { n, lines } = std::mem::replace(&mut self.state, State::Editing)
                else {
                    unreachable!()
                };
                let block: Vec<String> = lines.iter().map(|l| format!("L {l:08X}")).collect();
                self.printer.send(&block.join("\n"))?;
                self.state = State::Printing {
                    n,
                    done: 0,
                    total: lines.len() as u32,
                };
            }
            State::Printing { done, .. } if line.starts_with("P ") => {
                if let Some(i) = line[2..].split('/').next().and_then(|i| i.parse().ok()) {
                    *done = i;
                }
            }
            State::Printing { n, .. } if line == "DONE" && self.head.cut >= 2 => {
                let n = *n;
                let duration = self.label_start.elapsed().as_secs_f32();
                if n < self.number(END)? && !self.stop_requested {
                    self.note(format!("Label {n} printed in {duration:.1} s"));
                    self.launch(n + 1)?;
                } else if self.k_sent == 0 {
                    self.note(format!(
                        "Label {n} printed in {duration:.1} s: feeding out and cutting"
                    ));
                    self.printer.send("FLUSH")?;
                    self.state = State::Flushing { n };
                } else {
                    self.note(format!(
                        "Label {n} printed and cut in {duration:.1} s: series done"
                    ));
                    self.state = State::Editing;
                }
            }
            State::Flushing { .. } if line == "DONE" => {
                self.note("Series stopped, last label cut");
                self.state = State::Editing;
            }
            State::Printing { n, .. } if line == "DONE" => {
                let n = *n;
                let duration = self.label_start.elapsed().as_secs_f32();
                if n < self.number(END)? && !self.stop_requested {
                    self.note(format!(
                        "Label {n} out in {duration:.1} s: cut it, then press Enter"
                    ));
                    self.state = State::AwaitingCut { n };
                } else {
                    self.note(format!(
                        "Label {n} out in {duration:.1} s: series done"
                    ));
                    self.state = State::Editing;
                }
            }
            _ => {}
        }
        Ok(())
    }

    /// Keys of the saved templates list
    fn on_picker_key(&mut self, index: usize, key: KeyEvent) -> Result<()> {
        let count = self.registry.templates.len();
        match key.code {
            KeyCode::Up => self.picker = Some(index.saturating_sub(1)),
            KeyCode::Down => self.picker = Some((index + 1).min(count.saturating_sub(1))),
            KeyCode::Enter => {
                if let Some(t) = self.registry.templates.get(index) {
                    self.fields[TEMPLATE] = t.clone();
                    self.focus = TEMPLATE;
                    self.selection = true;
                }
                self.picker = None;
            }
            KeyCode::Delete | KeyCode::Backspace => {
                let t = self
                    .registry
                    .templates
                    .get(index)
                    .cloned()
                    .unwrap_or_default();
                self.registry.remove(index)?;
                self.note(format!("Template deleted: {t}"));
                self.picker = match self.registry.templates.len() {
                    0 => None,
                    n => Some(index.min(n - 1)),
                };
            }
            KeyCode::Esc => self.picker = None,
            KeyCode::Char('c') if key.modifiers.contains(KeyModifiers::CONTROL) => {
                self.quit = true
            }
            _ => {}
        }
        Ok(())
    }

    fn on_key(&mut self, key: KeyEvent) -> Result<()> {
        if let Some(index) = self.picker {
            return self.on_picker_key(index, key);
        }
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        match (&self.state, key.code) {
            (_, KeyCode::Char('c')) if ctrl => self.quit = true,

            (State::Editing, KeyCode::Esc) => self.quit = true,
            (State::Editing, KeyCode::Char('p')) | (State::Editing, KeyCode::F(5))
                if ctrl || key.code == KeyCode::F(5) =>
            {
                self.start_series()?
            }
            (State::Editing, KeyCode::Char('r')) if ctrl => {
                self.mirror = !self.mirror;
                self.note(if self.mirror {
                    "Mirrored"
                } else {
                    "Not mirrored"
                });
            }
            // Ctrl-N: new line in the template (the | bar cannot be typed
            // in every macOS terminal, where ⌥ does not always reach the program)
            (State::Editing, KeyCode::Char('n')) if ctrl => {
                self.focus = TEMPLATE;
                self.selection = false;
                self.fields[TEMPLATE].push(render::SEPARATOR);
            }
            (State::Editing, KeyCode::Char('s')) if ctrl => {
                let template = self.fields[TEMPLATE].clone();
                if self.registry.add(&template)? {
                    self.note(format!("Template saved: {template}"));
                } else {
                    self.note("Template already saved (or empty)");
                }
            }
            (State::Editing, KeyCode::Char('o')) if ctrl => {
                if self.registry.templates.is_empty() {
                    self.note("No saved templates: Ctrl-S saves the current one");
                } else {
                    let current = self
                        .registry
                        .templates
                        .iter()
                        .position(|t| *t == self.fields[TEMPLATE]);
                    self.picker = Some(current.unwrap_or(0));
                }
            }
            (State::Editing, KeyCode::Char('a')) if ctrl => {
                self.continuous = !self.continuous;
                self.note(if self.continuous {
                    "Continuous printing"
                } else {
                    "Stop at each line"
                });
            }
            (State::Editing, KeyCode::Tab | KeyCode::Down | KeyCode::Enter) => {
                self.focus = (self.focus + 1) % FIELDS.len();
                self.selection = true;
            }
            (State::Editing, KeyCode::BackTab | KeyCode::Up) => {
                self.focus = (self.focus + FIELDS.len() - 1) % FIELDS.len();
                self.selection = true;
            }
            (State::Editing, KeyCode::Backspace) => {
                if std::mem::take(&mut self.selection) {
                    self.fields[self.focus].clear();
                } else {
                    self.fields[self.focus].pop();
                }
            }
            (State::Editing, KeyCode::Char(c)) if !ctrl => {
                if std::mem::take(&mut self.selection) {
                    self.fields[self.focus].clear();
                }
                self.fields[self.focus].push(c);
            }

            (State::AwaitingCut { n }, KeyCode::Enter) => {
                let next = n + 1;
                self.launch(next)?
            }
            (State::AwaitingCut { .. }, KeyCode::Esc) => {
                self.note("Series stopped");
                self.state = State::Editing;
            }
            (State::Sending { .. } | State::Printing { .. }, KeyCode::Esc) => {
                self.stop_requested = true;
                self.note("Stopping after this label");
            }
            _ => {}
        }
        Ok(())
    }

    fn run(&mut self, terminal: &mut DefaultTerminal) -> Result<()> {
        while !self.quit {
            terminal.draw(|f| ui::draw(f, self))?;

            while let Ok(event) = self.rx.try_recv() {
                let result = match event {
                    printer::Event::Line(l) => self.on_line(&l),
                    printer::Event::Disconnected(e) => {
                        self.state = State::Editing;
                        Err(anyhow::anyhow!("board disconnected: {e}"))
                    }
                };
                if let Err(e) = result {
                    self.note(format!("Error: {e:#}"));
                }
            }

            if event::poll(Duration::from_millis(50))?
                && let Event::Key(key) = event::read()?
                && key.kind == KeyEventKind::Press
                && let Err(e) = self.on_key(key)
            {
                self.state = State::Editing;
                self.note(format!("Error: {e:#}"));
            }
        }
        Ok(())
    }
}

/// Character width: 1.08 gives the same length as the Dymo (42 mm instead of 39, 22/11/2026)
const DEFAULT_WIDTH: f32 = 1.08;

struct Arguments {
    port: Option<String>,
    font: PathBuf,
    mirror: bool,
    width: f32,
    templates: PathBuf,
}

impl Arguments {
    fn parse() -> Result<Self> {
        let mut args = Self {
            port: None,
            font: DEFAULT_FONT.into(),
            mirror: false,
            width: DEFAULT_WIDTH,
            templates: Registry::default_path(),
        };
        let mut iter = std::env::args().skip(1);
        while let Some(arg) = iter.next() {
            match arg.as_str() {
                "--port" => args.port = Some(iter.next().context("--port needs a value")?),
                "--font" => args.font = iter.next().context("--font needs a value")?.into(),
                "--mirror" => args.mirror = true,
                "--templates" => {
                    args.templates = iter.next().context("--templates needs a value")?.into()
                }
                "--width" => {
                    args.width = iter
                        .next()
                        .context("--width needs a value")?
                        .parse()
                        .context("--width: number expected, for example 1.08")?;
                    if !(1.0..=2.0).contains(&args.width) {
                        bail!("--width: between 1 and 2");
                    }
                }
                "-h" | "--help" => {
                    println!("{USAGE}");
                    std::process::exit(0);
                }
                other => bail!("unknown argument: {other}\n{USAGE}"),
            }
        }
        Ok(args)
    }
}

fn main() -> Result<()> {
    let args = Arguments::parse()?;
    let (tx, rx) = mpsc::channel();
    let name = match args.port {
        Some(p) => p,
        None => printer::find_port()?,
    };
    let mut printer = Printer::open(name, tx)?;
    let head = printer.request_info(&rx)?;
    let renderer = Renderer::load(&args.font, head.dots, args.width)?;
    let registry = Registry::load(args.templates)?;
    let template = registry
        .templates
        .first()
        .cloned()
        .unwrap_or_else(|| "ODO01{n:06}".into());
    let log = vec![
        format!("Font: {}", args.font.display()),
        format!(
            "Saved templates: {} ({})",
            registry.templates.len(),
            registry.path.display()
        ),
    ];

    let mut app = App {
        fields: [
            template,
            "581".into(),
            "582".into(),
            DEFAULT_HEAT_US.to_string(),
        ],
        focus: 0,
        selection: true,
        renderer,
        head,
        printer,
        rx,
        state: State::Editing,
        mirror: args.mirror,
        continuous: true, // V3: continuous is faster and just as sharp (22/11/2026)
        stop_requested: false,
        k_sent: 1,
        registry,
        picker: None,
        log,
        label_start: Instant::now(),
        quit: false,
    };

    let mut terminal = ratatui::init();
    let result = app.run(&mut terminal);
    ratatui::restore();
    result
}
