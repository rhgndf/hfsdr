#!/usr/bin/env python3
"""Create a Markdown summary of KiBot ERC/DRC reports."""

import argparse
import html.parser
import json
import re
from pathlib import Path
from typing import List, Optional


class TextExtractor(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.parts = []  # type: List[str]
        self.skip_depth = 0

    def handle_starttag(self, tag, attrs):
        if tag in {"script", "style"}:
            self.skip_depth += 1
        elif tag in {"br", "p", "div", "li", "tr", "h1", "h2", "h3", "h4"}:
            self.parts.append("\n")

    def handle_endtag(self, tag):
        if tag in {"script", "style"} and self.skip_depth:
            self.skip_depth -= 1
        elif tag in {"p", "div", "li", "tr", "h1", "h2", "h3", "h4"}:
            self.parts.append("\n")

    def handle_data(self, data):
        if not self.skip_depth:
            self.parts.append(data)

    def text(self):
        return "".join(self.parts)


class HtmlIssueExtractor(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_content_table = False
        self.table_depth = 0
        self.in_row = False
        self.in_cell = False
        self.current_cell = []  # type: List[str]
        self.current_cell_class = ""
        self.current_row = []  # type: List[dict]
        self.rows = []  # type: List[dict]

    def handle_starttag(self, tag, attrs):
        attrs_dict = dict(attrs)
        classes = set(attrs_dict.get("class", "").split())

        if tag == "table" and "content-table" in classes:
            self.in_content_table = True
            self.table_depth = 1
            return

        if self.in_content_table and tag == "table":
            self.table_depth += 1
        elif self.in_content_table and tag == "tr":
            self.in_row = True
            self.current_row = []
        elif self.in_row and tag == "td":
            self.in_cell = True
            self.current_cell = []
            self.current_cell_class = attrs_dict.get("class", "")
        elif self.in_cell and tag == "br":
            self.current_cell.append("\n")

    def handle_endtag(self, tag):
        if self.in_cell and tag == "td":
            text = re.sub(r"\s+", " ", "".join(self.current_cell)).strip()
            self.current_row.append({
                "text": text,
                "class": self.current_cell_class,
            })
            self.in_cell = False
            self.current_cell = []
            self.current_cell_class = ""
        elif self.in_row and tag == "tr":
            if len(self.current_row) >= 3:
                first_class = self.current_row[0]["class"].lower()
                if "td-error" in first_class or "td-warning" in first_class:
                    self.rows.append({
                        "severity": severity_from_class(first_class),
                        "code": self.current_row[0]["text"],
                        "description": self.current_row[1]["text"],
                        "location": self.current_row[2]["text"],
                    })
            self.in_row = False
            self.current_row = []
        elif self.in_content_table and tag == "table":
            self.table_depth -= 1
            if self.table_depth <= 0:
                self.in_content_table = False

    def handle_data(self, data):
        if self.in_cell:
            self.current_cell.append(data)


def severity_from_class(class_name):
    if "td-error" in class_name:
        return "Error"
    if "td-warning" in class_name:
        return "Warning"
    return "Info"


def html_issue_rows(path):
    parser = HtmlIssueExtractor()
    parser.feed(path.read_text(encoding="utf-8", errors="replace"))
    return parser.rows


def read_report(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix.lower() in {".html", ".htm"}:
        parser = TextExtractor()
        parser.feed(text)
        text = parser.text()
    return text


def normalize_lines(text):
    lines = []  # type: List[str]
    for raw_line in text.splitlines():
        line = re.sub(r"\s+", " ", raw_line).strip()
        if not line:
            continue
        line = re.sub(r"^\s*[•*-]\s*", "", line)
        lines.append(line)
    return lines


def looks_like_no_issues(lines):
    joined = "\n".join(lines).lower()
    has_nonzero_count = re.search(r"\b[1-9]\d*\s+(errors?|warnings?|violations?|issues?)\b", joined)
    if has_nonzero_count:
        return False
    no_issue_patterns = [
        r"\b0\s+errors?\b",
        r"\b0\s+warnings?\b",
        r"\b0\s+violations?\b",
        r"\b0\s+issues?\b",
        r"\bno\s+errors?\b",
        r"\bno\s+warnings?\b",
        r"\bno\s+violations?\b",
        r"\bno\s+issues?\b",
    ]
    return any(re.search(pattern, joined) for pattern in no_issue_patterns)


def issue_lines(lines):
    skip_patterns = [
        r"^$", r"^date\b", r"^time\b", r"^kicad\b", r"^report\b", r"^file\b",
        r"^items? checked\b", r"^checking\b", r"^running\b", r"^board\b", r"^schematic\b",
    ]
    issue_patterns = [
        r"\berror\b", r"\bwarning\b", r"\bviolation\b", r"\bunconnected\b",
        r"\bclearance\b", r"\boverlap\b", r"\bshort\b", r"\bnot connected\b",
        r"\bmissing\b", r"\bconflict\b", r"\bdrilled\b", r"\bannular\b",
    ]

    selected = []  # type: List[str]
    for line in lines:
        lowered = line.lower()
        if any(re.search(pattern, lowered) for pattern in skip_patterns):
            continue
        if any(re.search(pattern, lowered) for pattern in issue_patterns):
            selected.append(line)

    return selected


def severity_for(line):
    lowered = line.lower()
    if re.search(r"\berror\b|\bviolation\b|\bshort\b|\bclearance\b|\boverlap\b|\bannular\b", lowered):
        return "Error"
    if re.search(r"\bwarning\b|\bunconnected\b|\bnot connected\b|\bmissing\b|\bconflict\b", lowered):
        return "Warning"
    return "Info"


def markdown_cell(value):
    return value.replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")


def structured_issue_table(issues):
    rows = ["| # | Severity | Code | Description | Location |", "| ---: | --- | --- | --- | --- |"]
    for index, issue in enumerate(issues, start=1):
        rows.append(
            "| {} | {} | {} | {} | {} |".format(
                index,
                markdown_cell(issue["severity"]),
                markdown_cell(issue["code"]),
                markdown_cell(issue["description"]),
                markdown_cell(issue["location"]),
            )
        )
    return "\n".join(rows)


def issue_table(issues):
    rows = ["| # | Severity | Issue |", "| ---: | --- | --- |"]
    for index, issue in enumerate(issues, start=1):
        rows.append("| {} | {} | {} |".format(index, severity_for(issue), markdown_cell(issue)))
    return "\n".join(rows)


def summarize_report(path):
    if path.suffix.lower() in {".html", ".htm"}:
        html_issues = html_issue_rows(path)
        if html_issues:
            return structured_issue_table(html_issues), True
        return "No issues found in this report.", False

    lines = normalize_lines(read_report(path))
    if not lines or looks_like_no_issues(lines):
        return "No issues found in this report.", False

    issues = issue_lines(lines)
    if not issues:
        return "No issue rows found in this report. See the full HTML/text report for details.", False

    return issue_table(issues), bool(issues)


def find_report(report_dir, kind, basename):
    # type: (Path, str, str) -> Optional[Path]
    candidates = []
    for path in report_dir.rglob("*"):
        if not path.is_file():
            continue
        lowered = path.name.lower()
        if kind not in lowered:
            continue
        if path.suffix.lower() not in {".html", ".htm", ".txt", ".rpt", ".log", ".md"}:
            continue
        score = 0
        if basename.lower() in lowered:
            score -= 10
        if path.suffix.lower() in {".html", ".htm"}:
            score -= 3
        elif path.suffix.lower() in {".txt", ".rpt", ".log"}:
            score -= 2
        score += len(path.parts)
        candidates.append((score, path))
    if not candidates:
        return None
    return sorted(candidates, key=lambda item: (item[0], str(item[1])))[0][1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--projects-json", required=True)
    parser.add_argument("--artifacts-dir", default="check-artifacts")
    args = parser.parse_args()

    projects = json.loads(args.projects_json)
    artifacts_dir = Path(args.artifacts_dir)

    print("## ERC and DRC Issues")
    print()
    print("The summary below lists every detected ERC/DRC issue. Full HTML/text reports are in the check-report artifacts and on the review site when Pages is published.")
    print()

    for project in projects:
        slug = project["slug"]
        name = project["name"]
        basename = project.get("basename", slug)
        report_dir = artifacts_dir / f"{slug}-check-reports"

        print(f"### {name}")
        print()
        if not report_dir.exists():
            print("_No check-report artifact was found for this project._")
            print()
            continue

        for label, kind in (("ERC", "erc"), ("DRC", "drc")):
            report = find_report(report_dir, kind, basename)
            print(f"<details open><summary>{label}</summary>")
            print()
            if report is None:
                print(f"_No {label} report file was found in `{report_dir}`._")
            else:
                summary, _ = summarize_report(report)
                print(f"_Source: `{report.as_posix()}`_")
                print()
                print(summary)
            print()
            print("</details>")
            print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
