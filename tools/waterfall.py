import os
import subprocess
import cv2
import numpy as np

# Настройки
bin_file = "build/kernel.bin"
width = 264  # Ровно 66 пикселей * 4 байта (строка спрайта)
fps = 30

if not os.path.exists(bin_file):
  print(f"Ошибка: файл {bin_file} не найден!")
  exit(1)

# 1. Читаем весь kernel.bin как байты
with open(bin_file, "rb") as f:
  data = f.read()

# Выравниваем размер файла по ширине
padding = width - (len(data) % width) if len(data) % width != 0 else 0
data += b"\x00" * padding

# 2. Превращаем байты в массив пикселей (каждые 4 байта — это пиксель RGBA/BGRA)
arr = np.frombuffer(data, dtype=np.uint8)
# Изменяем форму в матрицу (высота, 66 пикселей, 4 байта на пиксель)
arr = arr.reshape((-1, 66, 4))

# Извлекаем оригинальные цвета (в uint32_t формате 0x00RRGGBB байты в памяти лежат как B, G, R, Alpha)
b = arr[:, :, 0]
g = arr[:, :, 1]
r = arr[:, :, 2]

# Собираем обратно в BGR-картинку для OpenCV
img = np.stack([b, g, r], axis=-1)

# Масштабируем пиксели крупно и четко (без размытия)
scale = 4
resized = cv2.resize(img, (66 * scale, img.shape[0] * scale), interpolation=cv2.INTER_NEAREST)

h, w, _ = resized.shape
video_height = 600
video_width = 800

# Создаем временное видео водопада
temp_video = "temp_native_waterfall.mp4"
fourcc = cv2.VideoWriter_fourcc(*"mp4v")
out = cv2.VideoWriter(temp_video, fourcc, fps, (video_width, video_height))

canvas = np.zeros((video_height, video_width, 3), dtype=np.uint8)
x_offset = (video_width - w) // 2
if x_offset < 0:
  x_offset = 0
  resized = resized[:, :video_width]
  w = video_width

# Плавный скролл водопада сверху донизу
scroll_y = -video_height
max_scroll = h

print(
    "Рендерим нативный водопад в оригинальных цветах (выискиваем Криса..."
    " отряд)..."
)
while scroll_y < max_scroll:
  canvas.fill(0)  # Черный фон
  src_y1 = max(0, scroll_y)
  src_y2 = min(h, scroll_y + video_height)
  dst_y1 = max(0, -scroll_y)
  dst_y2 = dst_y1 + (src_y2 - src_y1)

  if src_y2 > src_y1:
    canvas[dst_y1:dst_y2, x_offset : x_offset + w] = resized[src_y1:src_y2, :w]

  out.write(canvas)
  scroll_y += 6

out.release()

# 3. Сводим с тихим и безопасным для ушей звуком ядра (зациклено на всю длину, громкость 15%)
output_video = "kernel_kris_waterfall.mp4"
print("Сводим с тихим аудиопотоком ядра...")

# Заменяем ffmpeg_cmd в твоем скрипте на этот блок:
ffmpeg_cmd = [
    "ffmpeg",
    "-y",
    "-i",
    temp_video,
    "-stream_loop",
    "-1",  # Зацикливаем звук на всю длину видео
    "-f",
    "s16le",
    "-ar",
    "44100",
    "-ac",
    "1",
    "-i",
    bin_file,
    "-c:v",
    "libx264",
    "-pix_fmt",
    "yuv420p",
    "-c:a",
    "aac",
    # Убираем atempo.
    # volume=0.2 — комфортная громкость,
    # lowpass=f=800 — срезает весь резкий пищащий мусор, оставляя бархатный низкий гул,
    # aecho=0.8:0.9:500:0.4 — добавляет пространственное эхо, превращая бинарник в глубокий эмбиент.
    "-filter:a",
    "volume=0.2,lowpass=f=800,aecho=0.8:0.9:500:0.4",
    "-shortest",
    output_video,
]

subprocess.run(ffmpeg_cmd)

if os.path.exists(temp_video):
  os.remove(temp_video)

print(
    f"Готово! Финальный ролик сохранен как '{output_video}'. Теперь Крис вынырнет"
    " в своих родных цветах!"
)
