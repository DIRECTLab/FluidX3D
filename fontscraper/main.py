import re
import time
import shutil
import argparse
from pathlib import Path

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC


URL = "https://www.mxs.de/textstl/"


# -------------------------------------------------
# CLI
# -------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description="Batch generate STL files from TTF fonts and phrases.")
    p.add_argument("--fonts", required=True, type=Path, help="Directory containing .ttf files (recursed)")
    p.add_argument("--output", required=True, type=Path, help="Directory to write STL files")
    p.add_argument("--tmp", required=True, type=Path, help="Temporary download directory")
    p.add_argument("--phrases", required=True, type=Path, help="Path to phrases.txt (one phrase per line)")
    p.add_argument("--headless", action="store_true", help="Run Firefox headless")
    p.add_argument("--download-timeout", type=int, default=180, help="Seconds to wait for each STL download")
    p.add_argument("--render-sleep", type=float, default=0.4, help="Seconds to sleep after setting phrase")
    return p.parse_args()


# -------------------------------------------------
# Utility
# -------------------------------------------------

def sanitize_stem(name: str, max_len: int = 80) -> str:
    name = name.strip()
    name = re.sub(r"[^\w\-\.]+", "_", name)
    name = re.sub(r"_+", "_", name)
    name = name.strip("_") or "item"
    return name[:max_len]


def load_phrases(path: Path) -> list[str]:
    if not path.exists():
        print(f"[ERROR] Phrase file not found: {path}")
        return []
    phrases = []
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        phrases.append(s)
    if not phrases:
        print("[WARNING] Phrase file contains no usable phrases.")
    return phrases


def iter_ttf_fonts(font_dir: Path):
    # Warn-and-skip non-ttf
    for p in sorted(font_dir.rglob("*")):
        if not p.is_file():
            continue
        if p.suffix.lower() != ".ttf":
            print(f"[WARNING] Skipping non-TTF file: {p}")
            continue
        yield p


# -------------------------------------------------
# Selenium Setup
# -------------------------------------------------

def make_driver(download_dir: Path, headless: bool) -> webdriver.Firefox:
    download_dir.mkdir(parents=True, exist_ok=True)

    options = Options()
    if headless:
        options.add_argument("-headless")

    options.set_preference("browser.download.folderList", 2)
    options.set_preference("browser.download.dir", str(download_dir.resolve()))
    options.set_preference("browser.download.useDownloadDir", True)
    options.set_preference("browser.download.manager.showWhenStarting", False)
    options.set_preference("browser.download.alwaysOpenPanel", False)

    # STL downloads can come back under different types; include a few common ones
    options.set_preference(
        "browser.helperApps.neverAsk.saveToDisk",
        ",".join(
            [
                "application/sla",
                "application/vnd.ms-pki.stl",
                "model/stl",
                "model/x.stl-ascii",
                "application/octet-stream",
            ]
        ),
    )
    options.set_preference("pdfjs.disabled", True)
    options.set_preference("dom.webnotifications.enabled", False)

    driver = webdriver.Firefox(options=options)
    driver.set_page_load_timeout(60)
    return driver


def wait_for_ui_ready(driver, timeout_s: int = 30):
    wait = WebDriverWait(driver, timeout_s)
    wait.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[type='file']")))
    wait.until(EC.presence_of_element_located((By.XPATH, "//label[normalize-space(.)='Text']")))
    wait.until(EC.element_to_be_clickable((By.XPATH, "//button[contains(., 'download')]")))


def upload_font(driver, font_path: Path, timeout_s: int = 30):
    wait = WebDriverWait(driver, timeout_s)
    file_input = wait.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[type='file']")))
    file_input.send_keys(str(font_path.resolve()))
    # Let the app ingest the font and rerender
    time.sleep(0.8)


def set_phrase(driver, phrase: str, render_sleep: float):
    # Target the text input associated with the "Text" label
    text_input = driver.find_element(
        By.XPATH,
        "//label[normalize-space(.)='Text']/following::input[@type='text'][1]"
    )
    text_input.click()
    text_input.send_keys(Keys.CONTROL, "a")
    text_input.send_keys(Keys.BACKSPACE)
    text_input.send_keys(phrase)
    time.sleep(render_sleep)


def click_download(driver):
    driver.find_element(By.XPATH, "//button[contains(., 'download')]").click()


# -------------------------------------------------
# Download Handling
# -------------------------------------------------

def list_files(download_dir: Path) -> set[Path]:
    return {p for p in download_dir.glob("*") if p.is_file()}


def is_partial(p: Path) -> bool:
    return p.name.endswith(".part") or p.suffix.lower() in {".part", ".tmp", ".crdownload"}


def wait_for_new_download(download_dir: Path, before: set[Path], timeout_s: int = 180) -> Path:
    deadline = time.time() + timeout_s
    candidate = None
    last_size = None
    stable_since = None

    while time.time() < deadline:
        after = list_files(download_dir)
        new = [p for p in (after - before) if p.is_file() and not is_partial(p)]
        if not new:
            time.sleep(0.2)
            continue

        candidate = max(new, key=lambda p: p.stat().st_mtime)

        size = candidate.stat().st_size
        now = time.time()
        if last_size != size:
            last_size = size
            stable_since = now
        else:
            if stable_since is not None and (now - stable_since) >= 1.0:
                return candidate

        time.sleep(0.2)

    raise TimeoutError("Download did not complete in time")


def safe_move(src: Path, dst: Path) -> Path:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists():
        shutil.move(str(src), str(dst))
        return dst

    stem, suffix = dst.stem, dst.suffix
    for i in range(1, 10000):
        cand = dst.with_name(f"{stem}_{i:02d}{suffix}")
        if not cand.exists():
            shutil.move(str(src), str(cand))
            return cand

    raise RuntimeError(f"Could not generate unique filename for {dst}")


# -------------------------------------------------
# Main optimized flow
# -------------------------------------------------

def main():
    args = parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    args.tmp.mkdir(parents=True, exist_ok=True)

    phrases = load_phrases(args.phrases)
    if not phrases:
        print("[ERROR] No valid phrases loaded. Exiting.")
        return

    driver = make_driver(args.tmp, args.headless)

    try:
        for font_path in iter_ttf_fonts(args.fonts):
            try:
                # Load once per font
                driver.get(URL)
                wait_for_ui_ready(driver)
                upload_font(driver, font_path)

                # Iterate phrases without reloading
                for phrase in phrases:
                    try:
                        set_phrase(driver, phrase, args.render_sleep)

                        before = list_files(args.tmp)
                        click_download(driver)

                        downloaded = wait_for_new_download(args.tmp, before, timeout_s=args.download_timeout)

                        out_name = f"{sanitize_stem(font_path.stem)}__{sanitize_stem(phrase)}.stl"
                        final_path = safe_move(downloaded, args.output / out_name)

                        print(f"[OK] {font_path.name} + '{phrase}' -> {final_path.name}")

                    except Exception as e:
                        print(f"[ERROR] {font_path.name} + '{phrase}': {e}")

            except Exception as e:
                print(f"[ERROR] Failed processing font {font_path}: {e}")

    finally:
        driver.quit()


if __name__ == "__main__":
    main()
