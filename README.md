# Paint Tool SAI Gradients

Companion app for [Paint Tool SAI](https://www.systemax.jp/en/sai/) that approximates Photoshop **Gradient Map**. SAI has no plugin API, so this is a separate window next to SAI — not a DLL inside `sai2.exe`.

## Workflow

1. In SAI, copy a layer (`Ctrl+C`).
2. In this app, paste it (`Ctrl+V`) or press the global hotkey `Ctrl+Alt+G`.
3. Edit the gradient. The wipe slider compares original vs mapped.
4. Copy the result back (`Ctrl+C` here) and paste into SAI on a **new** layer (`Ctrl+V`).

You can also open / drag-and-drop PNG, JPEG, BMP, or TGA. If SAI clipboard drops transparency, export the layer as PNG and open the file.

## Build (Windows)

Needs CMake, Git, and Visual Studio 2022 with C++.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe is `build/Release/SaiGradientMap.exe`.

## Controls

| Action | Shortcut |
| --- | --- |
| Grab layer from clipboard | `Ctrl+V` or `Ctrl+Alt+G` |
| Copy mapped result | `Ctrl+C` |
| Open image | `Ctrl+O` |
| Save PNG | `Ctrl+S` |
| Delete selected stop | `Delete` |

Click the gradient bar to add a stop, drag markers to move them, right-click to remove. Luma defaults to Photoshop weights (`0.30 / 0.59 / 0.11`).

## License

Apache License 2.0. See [LICENSE](LICENSE).
