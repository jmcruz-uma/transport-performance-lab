#!/usr/bin/env python3

import argparse
import csv
import json
import os
import sys
from collections import defaultdict
from datetime import datetime

import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages


STAT_PREFIXES = [
    "elapsed_s",
    "elapsed_ms",
    "energy_j_raw",
    "idle_energy_j_estimated",
    "energy_j",
    "throughput_mib_s",
    "downloads_per_process",
    "total_iterations",
    "success",
    "failed",
]

STAT_KEYS = ["count", "mean", "median", "stdev", "p25", "p50", "p95", "min", "max"]

LIBRARY_DISPLAY_NAMES = {
    "bsd-sockets": "BSD-sockets",
    "asio": "ASIO",
    "capy-corosio": "Capy-Corosio",
    "async-berkeley": "Senders/Receivers",
    "taps-asio": "TAPS",
}

LIBRARY_ORDER = [
    "bsd-sockets",
    "asio",
    "capy-corosio",
    "async-berkeley",
    "taps-asio",
]

LIBRARY_DISPLAY_ORDER = [
    LIBRARY_DISPLAY_NAMES[name] for name in LIBRARY_ORDER
]

LIBRARY_COLORS = {
    "BSD-sockets": "tab:blue",
    "ASIO": "tab:orange",
    "Capy-Corosio": "tab:green",
    "Senders/Receivers": "tab:red",
    "TAPS": "tab:purple",
}


def library_display_name(library_id):
    return LIBRARY_DISPLAY_NAMES.get(library_id, library_id)


def library_sort_key(library_id):
    if library_id in LIBRARY_ORDER:
        return (LIBRARY_ORDER.index(library_id), library_id)
    return (len(LIBRARY_ORDER), library_id or "")


def library_display_sort_key(library_name):
    if library_name in LIBRARY_DISPLAY_ORDER:
        return (LIBRARY_DISPLAY_ORDER.index(library_name), library_name)
    return (len(LIBRARY_DISPLAY_ORDER), library_name or "")


def library_color(library_name):
    return LIBRARY_COLORS.get(library_name, None)


def ensure_parent_dir(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def flatten_stats(prefix, stats, row):
    if not stats:
        for key in STAT_KEYS:
            row[f"{prefix}_{key}"] = None
        return

    for key in STAT_KEYS:
        row[f"{prefix}_{key}"] = stats.get(key)


def safe_div(num, den):
    if num is None or den in (None, 0):
        return None
    return num / den


def fmt(v, decimals=4):
    if v is None:
        return "-"
    if isinstance(v, int):
        return str(v)
    return f"{v:.{decimals}f}"


def infer_library_name(path):
    base = os.path.basename(path)

    if base.endswith("_summary.json"):
        return base[:-len("_summary.json")]

    return os.path.splitext(base)[0]


def collect_summary_files(input_dir):
    summary_files = []

    for root, _, files in os.walk(input_dir):
        for name in sorted(files):
            if name.endswith("_summary.json"):
                summary_files.append(os.path.join(root, name))

    return sorted(summary_files, key=lambda path: library_sort_key(infer_library_name(path)))


def ensure_elapsed_ms_fields(row):
    for key in STAT_KEYS:
        ms_key = f"elapsed_ms_{key}"
        s_key = f"elapsed_s_{key}"
        if row.get(ms_key) is None and row.get(s_key) is not None:
            row[ms_key] = row[s_key] * 1000.0


def build_master_rows(summary_files):
    rows = []

    for path in summary_files:
        library_id = infer_library_name(path)
        library_name = library_display_name(library_id)

        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        file_served = data.get("file_served")
        host = data.get("host")
        repetitions = data.get("repetitions")
        ports = data.get("ports", {})
        compilers = data.get("compilers", [])

        for item in data.get("summary", []):
            compiler = item.get("compiler")
            server_threads = item.get("server_threads")
            parallel_bench_processes = item.get("parallel_bench_processes")

            row = {
                "library": library_name,
                "library_id": library_id,
                "compiler": compiler,
                "server_threads": server_threads,
                "parallel_bench_processes": parallel_bench_processes,
                "runs": item.get("runs"),
                "file_served": file_served,
                "host": host,
                "repetitions": repetitions,
                "configured_port": ports.get(compiler) if isinstance(ports, dict) else None,
                "available_compilers": ",".join(compilers) if compilers else None,
            }

            for prefix in STAT_PREFIXES:
                flatten_stats(prefix, item.get(f"{prefix}_stats"), row)

            ensure_elapsed_ms_fields(row)

            row["throughput_per_joule_mean"] = safe_div(
                row.get("throughput_mib_s_mean"),
                row.get("energy_j_mean"),
            )

            row["throughput_per_joule_median"] = safe_div(
                row.get("throughput_mib_s_median"),
                row.get("energy_j_median"),
            )

            row["downloads_per_second_mean"] = safe_div(
                row.get("downloads_per_process_mean"),
                row.get("elapsed_s_mean"),
            )

            rows.append(row)

    rows.sort(
        key=lambda x: (
            library_sort_key(x.get("library_id")),
            x["compiler"] or "",
            x["server_threads"] if x["server_threads"] is not None else -1,
            x["parallel_bench_processes"] if x["parallel_bench_processes"] is not None else -1,
        )
    )

    return rows


def build_best_by_case(rows):
    grouped = defaultdict(list)

    for row in rows:
        key = (
            row.get("compiler"),
            row.get("server_threads"),
            row.get("parallel_bench_processes"),
        )
        grouped[key].append(row)

    result = []

    for (compiler, server_threads, parallel_bench_processes), items in sorted(grouped.items()):
        best_time = min(
            items,
            key=lambda x: float("inf") if x.get("elapsed_ms_mean") is None else x["elapsed_ms_mean"]
        )
        best_energy = min(
            items,
            key=lambda x: float("inf") if x.get("energy_j_mean") is None else x["energy_j_mean"]
        )
        best_throughput = max(
            items,
            key=lambda x: float("-inf") if x.get("throughput_mib_s_mean") is None else x["throughput_mib_s_mean"]
        )
        best_efficiency = max(
            items,
            key=lambda x: float("-inf") if x.get("throughput_per_joule_mean") is None else x["throughput_per_joule_mean"]
        )

        result.append({
            "compiler": compiler,
            "server_threads": server_threads,
            "parallel_bench_processes": parallel_bench_processes,
            "best_time_library": best_time.get("library"),
            "best_time_library_id": best_time.get("library_id"),
            "best_time_elapsed_ms_mean": best_time.get("elapsed_ms_mean"),
            "best_energy_library": best_energy.get("library"),
            "best_energy_library_id": best_energy.get("library_id"),
            "best_energy_j_mean": best_energy.get("energy_j_mean"),
            "best_throughput_library": best_throughput.get("library"),
            "best_throughput_library_id": best_throughput.get("library_id"),
            "best_throughput_mib_s_mean": best_throughput.get("throughput_mib_s_mean"),
            "best_efficiency_library": best_efficiency.get("library"),
            "best_efficiency_library_id": best_efficiency.get("library_id"),
            "best_throughput_per_joule_mean": best_efficiency.get("throughput_per_joule_mean"),
        })

    return result


def build_library_overview(rows):
    grouped = defaultdict(list)

    for row in rows:
        grouped[row["library_id"]].append(row)

    overview = []

    for library_id, items in sorted(grouped.items(), key=lambda kv: library_sort_key(kv[0])):
        def collect(field):
            return [x[field] for x in items if x.get(field) is not None]

        elapsed_values = collect("elapsed_ms_mean")
        energy_values = collect("energy_j_mean")
        throughput_values = collect("throughput_mib_s_mean")
        efficiency_values = collect("throughput_per_joule_mean")

        overview.append({
            "library": library_display_name(library_id),
            "library_id": library_id,
            "cases": len(items),
            "average_elapsed_ms_mean": sum(elapsed_values) / len(elapsed_values) if elapsed_values else None,
            "average_energy_j_mean": sum(energy_values) / len(energy_values) if energy_values else None,
            "average_throughput_mib_s_mean": sum(throughput_values) / len(throughput_values) if throughput_values else None,
            "average_throughput_per_joule_mean": sum(efficiency_values) / len(efficiency_values) if efficiency_values else None,
            "best_elapsed_ms_mean": min(elapsed_values) if elapsed_values else None,
            "best_energy_j_mean": min(energy_values) if energy_values else None,
            "best_throughput_mib_s_mean": max(throughput_values) if throughput_values else None,
            "best_throughput_per_joule_mean": max(efficiency_values) if efficiency_values else None,
        })

    return overview


def write_json(rows, best_by_case, library_overview, output_path):
    ensure_parent_dir(output_path)

    payload = {
        "rows": rows,
        "best_by_case": best_by_case,
        "library_overview": library_overview,
    }

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=4)


def write_csv(rows, output_path):
    ensure_parent_dir(output_path)

    if not rows:
        with open(output_path, "w", newline="", encoding="utf-8") as f:
            pass
        return

    fieldnames = list(rows[0].keys())

    with open(output_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def add_text_page(pdf, title, lines, fontsize=11):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.clf()
    plt.axis("off")

    y = 0.97
    plt.figtext(0.05, y, title, fontsize=16, fontweight="bold", ha="left", va="top")
    y -= 0.04

    for line in lines:
        plt.figtext(0.05, y, line, fontsize=fontsize, ha="left", va="top", family="monospace")
        y -= 0.025
        if y < 0.05:
            pdf.savefig(fig, bbox_inches="tight")
            plt.close(fig)
            fig = plt.figure(figsize=(8.27, 11.69))
            fig.clf()
            plt.axis("off")
            y = 0.97

    pdf.savefig(fig, bbox_inches="tight")
    plt.close(fig)


def add_table_page(pdf, title, columns, rows, fontsize=8, scale_y=2.0, col_widths=None):
    fig, ax = plt.subplots(figsize=(19, 11.5))
    ax.axis("off")
    ax.set_title(title, fontsize=15, fontweight="bold", pad=18)

    table = ax.table(
        cellText=rows,
        colLabels=columns,
        loc="center",
        cellLoc="left",
        colLoc="center",
        colWidths=col_widths,
    )

    table.auto_set_font_size(False)
    table.set_fontsize(fontsize)
    table.scale(1, scale_y)

    for (row, col), cell in table.get_celld().items():
        cell.set_linewidth(0.8)
        if row == 0:
            cell.set_text_props(weight="bold", fontsize=fontsize + 0.5)
            cell.set_height(cell.get_height() * 1.25)
        else:
            cell.set_text_props(va="center")

    pdf.savefig(fig, bbox_inches="tight")
    plt.close(fig)


def add_image_page(pdf, image_path, title):
    fig = plt.figure(figsize=(8.27, 11.69))
    plt.axis("off")
    img = plt.imread(image_path)
    plt.imshow(img)
    plt.title(title, fontsize=14)
    pdf.savefig(fig, bbox_inches="tight")
    plt.close(fig)


def build_overview_rows(library_overview):
    rows = []

    for item in library_overview:
        rows.append([
            item["library"],
            str(item["cases"]),
            fmt(item["average_elapsed_ms_mean"]),
            fmt(item["average_energy_j_mean"]),
            fmt(item["average_throughput_mib_s_mean"]),
            fmt(item["average_throughput_per_joule_mean"]),
            fmt(item["best_elapsed_ms_mean"]),
            fmt(item["best_energy_j_mean"]),
            fmt(item["best_throughput_mib_s_mean"]),
            fmt(item["best_throughput_per_joule_mean"]),
        ])

    return rows


def build_best_case_rows(best_by_case):
    rows = []

    for item in best_by_case:
        rows.append([
            item["compiler"],
            str(item["server_threads"]),
            str(item["parallel_bench_processes"]),
            item["best_time_library"],
            fmt(item["best_time_elapsed_ms_mean"]),
            item["best_energy_library"],
            fmt(item["best_energy_j_mean"]),
            item["best_throughput_library"],
            fmt(item["best_throughput_mib_s_mean"]),
            item["best_efficiency_library"],
            fmt(item["best_throughput_per_joule_mean"]),
        ])

    return rows


def unique_values(rows, key):
    values = {row.get(key) for row in rows if row.get(key) is not None}

    if key == "library":
        return sorted(values, key=library_display_sort_key)

    return sorted(values)


def sanitize_filename(text):
    return str(text).replace(" ", "_").replace("/", "_").replace("\\", "_")


def plot_metric_all_libraries(rows, metric_field, ylabel, title, output_path):
    libraries = unique_values(rows, "library")

    plt.figure(figsize=(11, 7))

    for library in libraries:
        selected = [r for r in rows if r["library"] == library]
        selected.sort(key=lambda x: (x["parallel_bench_processes"], x["server_threads"], x["compiler"]))

        x = list(range(len(selected)))
        y = [r.get(metric_field) for r in selected]

        if any(v is not None for v in y):
            filtered_x = [xi for xi, yi in zip(x, y) if yi is not None]
            filtered_y = [yi for yi in y if yi is not None]
            plt.plot(
                filtered_x,
                filtered_y,
                marker="o",
                linewidth=2.0,
                label=library,
                color=library_color(library),
            )

    plt.xlabel("Ordered benchmark configuration index")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()


def plot_metric_grouped_by_case(rows, metric_field, ylabel, title, output_path):
    libraries = unique_values(rows, "library")
    cases = sorted({
        (r["compiler"], r["server_threads"], r["parallel_bench_processes"])
        for r in rows
    })

    x = list(range(len(cases)))

    plt.figure(figsize=(13, 7))

    for library in libraries:
        y = []
        for compiler, server_threads, parallel_clients in cases:
            match = next(
                (
                    r for r in rows
                    if r["library"] == library
                    and r["compiler"] == compiler
                    and r["server_threads"] == server_threads
                    and r["parallel_bench_processes"] == parallel_clients
                ),
                None,
            )
            y.append(match.get(metric_field) if match else None)

        filtered_x = [xi for xi, yi in zip(x, y) if yi is not None]
        filtered_y = [yi for yi in y if yi is not None]

        if filtered_y:
            plt.plot(
                filtered_x,
                filtered_y,
                marker="o",
                linewidth=2.0,
                label=library,
                color=library_color(library),
            )

    tick_labels = [
        f"{compiler}, T={server_threads}, C={parallel_clients}"
        for compiler, server_threads, parallel_clients in cases
    ]

    plt.xlabel("Benchmark case")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.xticks(x, tick_labels, rotation=45, ha="right")
    plt.grid(True, alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()


def plot_metric_by_compiler(rows, compiler, metric_field, ylabel, title, output_path):
    libraries = unique_values(rows, "library")
    selected_rows = [r for r in rows if r["compiler"] == compiler]

    cases = sorted({
        (r["server_threads"], r["parallel_bench_processes"])
        for r in selected_rows
    })

    x = list(range(len(cases)))

    plt.figure(figsize=(13, 7))

    for library in libraries:
        y = []
        for server_threads, parallel_clients in cases:
            match = next(
                (
                    r for r in selected_rows
                    if r["library"] == library
                    and r["server_threads"] == server_threads
                    and r["parallel_bench_processes"] == parallel_clients
                ),
                None,
            )
            y.append(match.get(metric_field) if match else None)

        filtered_x = [xi for xi, yi in zip(x, y) if yi is not None]
        filtered_y = [yi for yi in y if yi is not None]

        if filtered_y:
            plt.plot(
                filtered_x,
                filtered_y,
                marker="o",
                linewidth=2.0,
                label=library,
                color=library_color(library),
            )

    tick_labels = [f"T={server_threads}, C={parallel_clients}" for server_threads, parallel_clients in cases]

    plt.xlabel("Benchmark case")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.xticks(x, tick_labels, rotation=45, ha="right")
    plt.grid(True, alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()


def plot_metric_by_server_threads(rows, server_threads, metric_field, ylabel, title, output_path):
    libraries = unique_values(rows, "library")
    selected_rows = [r for r in rows if r["server_threads"] == server_threads]

    cases = sorted({
        (r["compiler"], r["parallel_bench_processes"])
        for r in selected_rows
    })

    x = list(range(len(cases)))

    plt.figure(figsize=(13, 7))

    for library in libraries:
        y = []
        for compiler, parallel_clients in cases:
            match = next(
                (
                    r for r in selected_rows
                    if r["library"] == library
                    and r["compiler"] == compiler
                    and r["parallel_bench_processes"] == parallel_clients
                ),
                None,
            )
            y.append(match.get(metric_field) if match else None)

        filtered_x = [xi for xi, yi in zip(x, y) if yi is not None]
        filtered_y = [yi for yi in y if yi is not None]

        if filtered_y:
            plt.plot(
                filtered_x,
                filtered_y,
                marker="o",
                linewidth=2.0,
                label=library,
                color=library_color(library),
            )

    tick_labels = [f"{compiler}, C={parallel_clients}" for compiler, parallel_clients in cases]

    plt.xlabel("Benchmark case")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.xticks(x, tick_labels, rotation=45, ha="right")
    plt.grid(True, alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()


def generate_global_comparison_plots(rows, plots_dir):
    os.makedirs(plots_dir, exist_ok=True)
    generated = []

    metric_specs = [
        ("elapsed_ms_mean", "Mean execution time (milliseconds)", "Global comparison of mean execution time"),
        ("elapsed_ms_p95", "95th percentile execution time (milliseconds)", "Global comparison of 95th percentile execution time"),
        ("energy_j_mean", "Mean net energy consumption (joules)", "Global comparison of mean net energy consumption"),
        ("energy_j_p95", "95th percentile net energy consumption (joules)", "Global comparison of 95th percentile net energy consumption"),
        ("throughput_mib_s_mean", "Mean aggregate throughput (MiB/s)", "Global comparison of mean aggregate throughput"),
        ("throughput_mib_s_p95", "95th percentile aggregate throughput (MiB/s)", "Global comparison of 95th percentile aggregate throughput"),
        ("throughput_per_joule_mean", "Mean throughput per joule (MiB/s/J)", "Global comparison of mean throughput per joule"),
    ]

    for metric_field, ylabel, title in metric_specs:
        out = os.path.join(plots_dir, f"{sanitize_filename(metric_field)}_all_cases.png")
        plot_metric_grouped_by_case(rows, metric_field, ylabel, title, out)
        generated.append((out, title))

    compilers = unique_values(rows, "compiler")
    for compiler in compilers:
        for metric_field, ylabel, title in metric_specs:
            out = os.path.join(plots_dir, f"{sanitize_filename(metric_field)}_{sanitize_filename(compiler)}.png")
            plot_metric_by_compiler(
                rows,
                compiler,
                metric_field,
                ylabel,
                f"{title} ({compiler})",
                out,
            )
            generated.append((out, f"{title} ({compiler})"))

    server_thread_values = unique_values(rows, "server_threads")
    for server_threads in server_thread_values:
        for metric_field, ylabel, title in metric_specs:
            out = os.path.join(plots_dir, f"{sanitize_filename(metric_field)}_threads_{server_threads}.png")
            plot_metric_by_server_threads(
                rows,
                server_threads,
                metric_field,
                ylabel,
                f"{title} (server threads = {server_threads})",
                out,
            )
            generated.append((out, f"{title} (server threads = {server_threads})"))

    return generated


def generate_global_pdf_report(rows, best_by_case, library_overview, pdf_path, plots_dir):
    generated_plots = generate_global_comparison_plots(rows, plots_dir)

    with PdfPages(pdf_path) as pdf:
        cover_lines = [
            "Global benchmark comparison report",
            f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            "",
            f"Number of libraries: {len(unique_values(rows, 'library'))}",
            f"Number of compilers: {len(unique_values(rows, 'compiler'))}",
            f"Server thread values: {unique_values(rows, 'server_threads')}",
            f"Parallel client values: {unique_values(rows, 'parallel_bench_processes')}",
            "",
            "This report compares all collected transport libraries together.",
            "It includes:",
            "- library-level overview tables",
            "- per-case winners",
            "- global metric comparison plots",
            "- compiler-specific comparison plots",
            "- server-thread-specific comparison plots",
            "",
            "Main comparison metrics:",
            "- mean execution time",
            "- 95th percentile execution time",
            "- mean net energy",
            "- 95th percentile net energy",
            "- mean throughput",
            "- 95th percentile throughput",
            "- mean throughput per joule",
        ]
        add_text_page(pdf, "Global benchmark comparison report", cover_lines, fontsize=11)

        overview_columns = [
            "Library",
            "Cases",
            "Avg. time mean (ms)",
            "Avg. energy mean",
            "Avg. throughput mean",
            "Avg. throughput/J mean",
            "Best time mean (ms)",
            "Best energy mean",
            "Best throughput mean",
            "Best throughput/J mean",
        ]
        overview_rows = build_overview_rows(library_overview)

        chunk_size = 8
        for i in range(0, len(overview_rows), chunk_size):
            add_table_page(
                pdf,
                f"Library overview (part {i // chunk_size + 1})",
                overview_columns,
                overview_rows[i:i + chunk_size],
                fontsize=8,
                scale_y=2.25,
                col_widths=[0.18, 0.07, 0.09, 0.09, 0.10, 0.11, 0.09, 0.09, 0.10, 0.11],
            )

        best_columns = [
            "Compiler",
            "Server threads",
            "Parallel clients",
            "Fastest library",
            "Fastest time mean (ms)",
            "Lowest-energy library",
            "Lowest energy mean",
            "Highest-throughput library",
            "Highest throughput mean",
            "Best-efficiency library",
            "Best throughput/J mean",
        ]
        best_rows = build_best_case_rows(best_by_case)

        chunk_size = 8
        for i in range(0, len(best_rows), chunk_size):
            add_table_page(
                pdf,
                f"Best library by case (part {i // chunk_size + 1})",
                best_columns,
                best_rows[i:i + chunk_size],
                fontsize=7.5,
                scale_y=2.2,
                col_widths=[0.08, 0.08, 0.08, 0.13, 0.10, 0.13, 0.10, 0.13, 0.10, 0.13, 0.10],
            )

        for image_path, title in generated_plots:
            if os.path.exists(image_path):
                add_image_page(pdf, image_path, title)


def main():
    parser = argparse.ArgumentParser(description="Build a global benchmark summary table and report.")
    parser.add_argument("--input-dir", required=True, help="Directory containing collected summary JSON files.")
    parser.add_argument("--json-out", required=True, help="Output JSON file.")
    parser.add_argument("--csv-out", required=True, help="Output CSV file.")
    parser.add_argument("--pdf-out", required=False, help="Optional global PDF comparison report.")
    parser.add_argument("--plots-dir", required=False, help="Optional directory for generated global plots.")
    parser.add_argument(
        "--exclude-from-plots",
        action="append",
        default=[],
        metavar="LIBRARY_ID",
        help=(
            "library_id to leave out of the PDF's plots/best-of tables only "
            "(repeatable). The JSON and CSV outputs always include every "
            "library -- this never discards data, it only keeps one library's "
            "outlier values from compressing everyone else's axis in a plot. "
            "E.g. capy-corosio's TLS runtime is a known ~20x+ outlier "
            "(openssl_stream lacking a compound read/write op; see "
            "design/tls_experiment_notes.md) -- excluded from tls/tls_framed "
            "plots for that reason, still fully present in the raw data for "
            "the separate corosio-upstream report."
        ),
    )
    args = parser.parse_args()

    if not os.path.isdir(args.input_dir):
        print(f"Error: input directory does not exist: {args.input_dir}", file=sys.stderr)
        sys.exit(1)

    summary_files = collect_summary_files(args.input_dir)
    if not summary_files:
        print("No *_summary.json files were found.", file=sys.stderr)
        sys.exit(1)

    rows = build_master_rows(summary_files)
    best_by_case = build_best_by_case(rows)
    library_overview = build_library_overview(rows)

    write_json(rows, best_by_case, library_overview, args.json_out)
    write_csv(rows, args.csv_out)

    print(f"Master JSON written to: {args.json_out}")
    print(f"Master CSV written to: {args.csv_out}")

    if args.pdf_out:
        plots_dir = args.plots_dir
        if not plots_dir:
            plots_dir = os.path.join(os.path.dirname(args.pdf_out), "global_comparison_plots")

        os.makedirs(os.path.dirname(args.pdf_out), exist_ok=True)
        os.makedirs(plots_dir, exist_ok=True)

        plot_rows = rows
        plot_best_by_case = best_by_case
        plot_library_overview = library_overview
        if args.exclude_from_plots:
            excluded = set(args.exclude_from_plots)
            plot_rows = [r for r in rows if r.get("library_id") not in excluded]
            plot_best_by_case = build_best_by_case(plot_rows)
            plot_library_overview = build_library_overview(plot_rows)
            print(f"Excluded from plots only (still in JSON/CSV): {sorted(excluded)}")

        generate_global_pdf_report(plot_rows, plot_best_by_case, plot_library_overview, args.pdf_out, plots_dir)

        print(f"Global PDF report written to: {args.pdf_out}")
        print(f"Global plots directory written to: {plots_dir}")


if __name__ == "__main__":
    main()