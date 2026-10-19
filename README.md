# WinTray

A lightweight, macOS-inspired "Dynamic Island" floating tray utility for Windows 10 and 11. Built entirely in native C++ (Win32 API) with acrylic glass blur effects, smooth kinetic spring animations, and multi-file drag-and-drop integration.

---

## Features

- **Dynamic Island / Notch UX:** Stays docked at the top center of your primary screen as a tiny 16px capsule. Expands smoothly downward into a frosted glass card when hovered.
- **Two-Section Organization:**
  - **Pinned Section (Left):** Items pinned via context menu stay persistent, denoted by a warm amber badge.
  - **Temporary Queue (Right):** Unpinned files sit in an interactive, horizontally scrollable shelf and automatically remove themselves after a drag-out.
- **Interactive Multi-Selection:**
  - **Single Click:** Selects an item with a blue indicator.
  - **Ctrl + Click:** Toggles individual items in/out of the selection.
  - **Shift + Click:** Selects a continuous range between items.
  - **Double Click:** Launches the selected file or application.
  - **Delete Key:** Removes selected items from the tray.
- **OLE Drag-and-Drop:**
  - Full bidirectional integration with Windows Explorer, Desktop, and web browsers.
  - Multi-drag support: Dragging any selected item bundles all currently selected items into the payload.
- **Batch Actions:** Right-click to compress selected files into a `.zip` archive using a custom name and destination dialog.
- **Settings & Accessibility Panel (⚙):**
  - Toggle between **Copy** or **Move** behavior on drag-out.
  - Toggle auto-removal of unpinned items post-drop.
  - Toggle hover magnification animation.
  - Toggle large high-visibility icons for high-DPI displays.
  - In-app update checker.
- **Zero Dependencies:** Compiles to a single standalone portable `.exe` with the MSVC runtime statically linked.

---

## Prerequisites for Building

- **Windows 10 / 11 (64-bit)**
- **Visual Studio 2022 / Build Tools** (with the *Desktop development with C++* workload installed)
- **CMake 3.20 or newer**

---

## Local Development & Compilation

### 1. Build a Standalone Release Binary

Open PowerShell in the root project directory:

```powershell
# Stop any running instances
Stop-Process -Name WinTray -Force -ErrorAction SilentlyContinue

# Generate project files and build with static runtime (/MT)
cmake -B build -A x64
cmake --build build --config Release



# WinTray — Release & Versioning Guide

This guide covers how to increment versions, create production release binaries, and publish updates to users using the built-in update mechanism.

---

## 1. Versioning Architecture

WinTray compares semantic version strings (e.g., `v1.0.0`, `v1.0.1`) against the public GitHub Releases API:

```text
[https://api.github.com/repos/](https://api.github.com/repos/)<yourusername>/wintray/releases/latest

```

* **Version Tag Format:** Always prefix tags with `v` (e.g., `v1.0.1`, `v1.1.0`).
* **Repository Visibility:** The releases repository must be **Public**. If the repository is set to Private, GitHub's API returns a `404 Not Found` to unauthenticated client requests, which disables the in-app update check.

---

## 2. Step-by-Step Release Workflow

### Step 1: Update the Version in Source Code

Open `src/main.cpp` and update `CURRENT_VERSION` to your target release tag:

```cpp
// src/main.cpp (around line 21)
const std::wstring CURRENT_VERSION = L"v1.0.1";
const std::wstring GITHUB_REPO = L"yourusername/wintray";

```

* Ensure `GITHUB_REPO` matches your exact GitHub username and repository name.

---

### Step 2: Compile the Release Executable

Open PowerShell in the project root directory and build using the Release configuration:

```powershell
# Terminate any running instances of WinTray
Stop-Process -Name WinTray -Force -ErrorAction SilentlyContinue

# Build the standalone Release binary
cmake --build build --config Release

```

**How to Verify:**

Check that the newly generated binary has the current timestamp:

```powershell
Get-Item .\build\Release\WinTray.exe | Select-Object LastWriteTime, Length

```

---

### Step 3: Commit and Push Changes to Git

Commit the version bump and push your updates to your remote repository:

```bash
git add src/main.cpp
git commit -m "Bump version to v1.0.1"
git push origin main

```

---

### Step 4: Publish the Release on GitHub

1. Open your repository on GitHub.
2. In the right-hand sidebar, navigate to **Releases** and click **Draft a new release**.
3. Click **Choose a tag**:
* Type the exact version tag defined in `CURRENT_VERSION` (e.g., `v1.0.1`).
* Click **Create new tag: v1.0.1 on publish**.


4. Set the **Release title** (e.g., `WinTray v1.0.1`).
5. *(Optional)* Provide changelog notes in the description box.
6. In the **Attach binaries by dropping them here or selecting them** area:
* Drag and drop `build\Release\WinTray.exe`.


7. Click **Publish release**.

**How to Verify:**

1. Launch an existing installed version of WinTray (e.g., `v1.0.0`).
2. Hover over the notch to expand the tray.
3. Click the ⚙ gear icon in the top right to open **Settings**.
4. Click **Check for Updates...**.
5. It should notify you that `v1.0.1` is available and prompt you to open the download page.

---

## 3. Distributing the Binary to Users

WinTray uses static MSVC runtime linking (`/MT`), producing a zero-dependency standalone binary.

* **Distribution File:** Share only `build\Release\WinTray.exe`.
* **Client Dependencies:** None. No Visual C++ Redistributable, .NET runtime, or installers are needed.
* **User Configuration Path:** User preferences are automatically created and stored per machine at:

```text
%APPDATA%\WinTray\WinTray.ini

```

### Optional: Set Up Run on Startup for Clients

1. Press `Win + R`, type `shell:startup`, and press **Enter**.
2. Right-click inside the folder, select **New -> Shortcut**, and browse to `WinTray.exe`.

```

```