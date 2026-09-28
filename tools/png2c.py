import sys
from PIL import Image

def convert_and_crop(image_path, output_header_path, var_name="logo_data"):
    try:
        img = Image.open(image_path).convert("RGB")
    except Exception as e:
        print(f"Ошибка открытия картинки: {e}")
        return

    # Автоматически находим и обрезаем черные поля вокруг логотипа
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)

    width, height = img.size
    pixels = img.load()

    with open(output_header_path, "w", encoding="utf-8") as f:
        f.write("#ifndef LOGO_H\n")
        f.write("#define LOGO_H\n\n")
        f.write(f"#define LOGO_WIDTH  {width}\n")
        f.write(f"#define LOGO_HEIGHT {height}\n\n")
        f.write(f"static const uint32_t {var_name}[] = {{\n")
        
        for y in range(height):
            f.write("    ")
            for x in range(width):
                r, g, b = pixels[x, y]
                color = (r << 16) | (g << 8) | b
                f.write(f"0x{color:06X}, ")
            f.write("\n")
            
        f.write("};\n\n")
        f.write("#endif\n")

    print(f"[+] Логотип успешно обрезан и конвертирован!")
    print(f"[+] Новый размер: {width}x{height} пикселей")
    print(f"[+] Файл сохранен как: {output_header_path}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Использование: python convert.py <картинка.png> logo.h")
    else:
        convert_and_crop(sys.argv[1], sys.argv[2])