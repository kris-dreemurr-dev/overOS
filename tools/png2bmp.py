from PIL import Image
Image.open("logo.png").convert("RGB").save("logo.bmp")