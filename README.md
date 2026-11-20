# CD-952 實體機電子書閱讀器 (Project Muteki)

本專案已依據 WoodyBoy 的 Project Muteki API 完成實機對接 (`platform_muteki.c`)，支援：
- **系統原生開檔視窗** (`_GetOpenFileName`)：程式與書籍隨便放 `Download` 或任何資料夾都能開啟。
- **系統原生中文字型** (`MONOSPACE_CJK` + `WriteAlignString`)：免外掛字庫檔，直接顯示繁體中文。
- **純 C 24-bit BMP 圖片顯示**：支援瀏覽 PDF 轉出的 `.bmp` 圖片書。
- **自動書籤**：翻頁自動建立 `.bmk` 記住進度。

## 如何編譯出 CD-952 實體機專用 `cd952-reader.exe`
需要安裝 Project Muteki 工具鏈 (`arm-none-bestaeabi`、`mutekix`、`romtool`、`elf2bestape`)，通常在 Linux / WSL 下透過 Project Muteki 官方 Flatpak SDK 執行：

```bash
meson setup build --cross-file arm-none-bestaeabi-arm9.ini
meson compile -C build
```

編譯完成後，`build/cd952-reader.exe` 就是可以直接丟進無敵 CD-952 `Download` 資料夾執行的實體機程式！

## 實體機操作按鍵
- `O`：開啟檔案選擇器 (`.txt` / `.bmp`)
- `方向鍵` / `PageUp` / `PageDown` / `空白鍵`：翻頁
- `T`：切換日間護眼 / 夜間深色模式
- `ESC`：退出程式
