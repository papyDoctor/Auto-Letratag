//! Numbered text templates: `BOX-{n:03}` gives BOX-001, BOX-002, ...

/// Replaces `{n}`, `{n:3}` (minimum width) or `{n:03}` (zero-padded) with the number.
pub fn format(template: &str, n: i64) -> String {
    let mut output = String::new();
    let mut rest = template;
    while let Some(start) = rest.find("{n") {
        let Some(end) = rest[start..].find('}').map(|f| start + f) else {
            break;
        };
        let spec = &rest[start + 2..end];
        let number = match spec.strip_prefix(':') {
            Some(width) if width.starts_with('0') => {
                format!("{n:0w$}", w = width.parse().unwrap_or(0))
            }
            Some(width) => format!("{n:w$}", w = width.parse().unwrap_or(0)),
            None if spec.is_empty() => n.to_string(),
            // `{name}` or anything else: left as is
            None => rest[start..=end].to_string(),
        };
        output.push_str(&rest[..start]);
        output.push_str(&number);
        rest = &rest[end + 1..];
    }
    output.push_str(rest);
    output
}

#[cfg(test)]
mod tests {
    use super::format;

    #[test]
    fn formats() {
        assert_eq!(format("BOX-{n:03}", 7), "BOX-007");
        assert_eq!(format("{n}/{n:4}", 12), "12/  12");
        assert_eq!(format("no number", 3), "no number");
        assert_eq!(format("{name} {n}", 3), "{name} 3");
        assert_eq!(format("open {n", 3), "open {n");
    }
}
