# Building Quanta

Quanta leverages the **XLang** execution engine and its memory handling primitives. To build Quanta successfully, the `xlang` repository must be present in the same parent directory.

```text
/your_workspace/
├── xlang/         <-- (Required dependency)
└── Quanta/        <-- (This repository)
```

## 1. Clone Dependencies

First, ensure you have the `xlang` runtime cloned into your workspace:

```bash
cd /your_workspace/
git clone https://github.com/CantorAI/xlang.git
```

Then, clone Quanta:

```bash
git clone https://github.com/CantorAI/Quanta.git
cd Quanta
```

## 2. Compile the Project

We provide automated build scripts for Windows, Linux, and macOS. Ensure you have `CMake` (3.10+) installed on your system.

### Windows
Run the batch script from the command prompt or double-click it:
```cmd
build.bat
```

### Linux / macOS
Make the shell script executable and run it:
```bash
chmod +x build.sh
./build.sh
```

### Manual Build
If you prefer to run CMake manually:
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```
