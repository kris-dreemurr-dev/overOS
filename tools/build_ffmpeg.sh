#!/bin/sh
# =============================================================================
# build_ffmpeg.sh — сборка настоящего FFmpeg под overOS в /lib/ffmpeg.ll.
#
# Что делает:
#   1. берёт исходники FFmpeg (архив из src/libs/ffmpeg/dist или скачивает его);
#   2. накладывает заплаты из src/libs/ffmpeg/patches;
#   3. собирает libffsys.a — «libc» overOS для FFmpeg (src/libs/ffmpeg/ffsys.c плюс
#      наши libc.c и libm.c): по ней ./configure и узнаёт, что у системы есть;
#   4. кросс-конфигурирует и собирает libavformat/libavcodec/libswscale/
#      libswresample компилятором-обёрткой src/libs/ffmpeg/ovcc (freestanding, свой
#      сисрут, никаких заголовков хозяйской системы);
#   5. линкует всё это вместе с прослойкой src/libs/ffmpeg/avshim.c в образ с базой 0
#      и упаковывает tools/elf2ll.py в .ll.
#
# Вызывается из Makefile (цель build/ffmpeg.ll), пересборка FFmpeg происходит
# один раз: дальше достаточно пересобрать прослойку.
#
# Переменные: OUT — куда положить .ll, BUILD — рабочий каталог,
# FFMPEG_VERSION, FFMPEG_TARBALL, FFMPEG_URL.
# =============================================================================
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/ffmpeg.ll}
BUILD=${BUILD:-$ROOT/build/ffmpeg}
VER=${FFMPEG_VERSION:-7.1}
TARBALL=${FFMPEG_TARBALL:-$ROOT/src/libs/ffmpeg/dist/ffmpeg-$VER.tar.xz}
URL=${FFMPEG_URL:-https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz}

SRC=$BUILD/src
OBJ=$BUILD/obj
FFB=$BUILD/ff

say() { printf '\033[36m[ffmpeg]\033[0m %s\n' "$1"; }

mkdir -p "$BUILD" "$OBJ"

# --- 1. исходники -------------------------------------------------------------
if [ ! -f "$SRC/configure" ]; then
    if [ ! -f "$TARBALL" ]; then
        say "нет архива $TARBALL, качаю $URL"
        mkdir -p "$(dirname "$TARBALL")"
        if command -v curl > /dev/null 2>&1; then
            curl -L --fail -o "$TARBALL.part" "$URL"
        else
            wget -O "$TARBALL.part" "$URL"
        fi
        mv "$TARBALL.part" "$TARBALL"
    fi
    say "распаковываю FFmpeg $VER"
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar xf "$TARBALL" -C "$SRC" --strip-components=1
    for p in "$ROOT"/src/libs/ffmpeg/patches/*.patch; do
        [ -f "$p" ] || continue
        say "заплата $(basename "$p")"
        ( cd "$SRC" && patch -p1 -s < "$p" )
    done
fi

# Ассемблерные (SIMD) ядра FFmpeg. Их стоит собирать: именно они дают
# декодированию H.264 несколько раз больше скорости, а на нашем ноутбуке
# (двухъядерный Haswell 1,7 ГГц) это разница между видео и слайд-шоу.
# FFMPEG_ASM=0 собирает по-старому, на чистом C, если нужен запасной вариант.
if [ "${FFMPEG_ASM:-1}" = "0" ]; then FF_ASM="--disable-asm"; else FF_ASM="--enable-x86asm"; fi

# --- 2. libc overOS для FFmpeg ------------------------------------------------
export OV_ROOT=$ROOT
export OV_SYSLIB=$OBJ/libffsys.a
CC=$ROOT/src/libs/ffmpeg/ovcc

say "собираю сисрут (ffsys.c + libc.c + libm.c)"
$CC -O2 -Wall -Wextra -c "$ROOT/src/libs/ffmpeg/ffsys.c" -o "$OBJ/ffsys.o"
$CC -O2 -DLIBC_NO_HEAP   -c "$ROOT/src/libs/libc/libc.c"  -o "$OBJ/libc.o"
$CC -O2                  -c "$ROOT/src/libs/libm/libm.c"  -o "$OBJ/libm.o"
rm -f "$OV_SYSLIB"
ar rcs "$OV_SYSLIB" "$OBJ/ffsys.o" "$OBJ/libc.o" "$OBJ/libm.o"

# --- 3. конфигурация и сборка самого FFmpeg -----------------------------------
if [ ! -f "$FFB/ffbuild/config.mak" ]; then
    say "конфигурирую FFmpeg под overOS"
    mkdir -p "$FFB"
    ( cd "$FFB" && PATH="$ROOT/src/libs/ffmpeg:$PATH" "$SRC/configure" \
        --cc=ovcc --enable-cross-compile --target-os=none --arch=x86_64 \
        --enable-static --disable-shared \
        --disable-programs --disable-doc --disable-autodetect \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-network --disable-protocols --disable-iconv \
        --disable-pthreads --disable-w32threads --disable-os2threads \
        $FF_ASM --disable-debug --disable-xlib --disable-sdl2 \
        --disable-everything \
        --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,wmv3,vc1,h263,h263p,flv,theora,prores,dvvideo,cinepak,rawvideo,mjpeg,png,bmp,gif,webp,apng \
        --enable-decoder=aac,aac_latm,mp1,mp1float,mp2,mp2float,mp3,mp3float,flac,vorbis,opus,alac,ac3,eac3,dca,truehd,mlp,wmav1,wmav2,wmapro,ape,tta,wavpack,nellymoser,gsm,amrnb,amrwb,ra_144,ra_288,sipr,qdm2,cook \
        --enable-decoder=pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_u16le,pcm_u16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_alaw,pcm_mulaw \
        --enable-decoder=adpcm_ima_qt,adpcm_ima_wav,adpcm_ms,adpcm_swf,adpcm_yamaha,adpcm_ima_dk3,adpcm_ima_dk4 \
        --enable-demuxer=mov,mp3,wav,aac,h264,hevc,matroska,avi,flv,live_flv,asf,ogg,flac,ape,tta,wv,mpegts,mpegtsraw,mpegps,mpegvideo,m4v,rm,caf,aiff,au,dsf,w64,voc,ac3,eac3,dts,amr,gif,image2,image2pipe,dv,swf,mjpeg,yuv4mpegpipe,wsd,pcm_s16le,pcm_u8,pcm_mulaw,pcm_alaw \
        --enable-parser=h264,hevc,aac,aac_latm,mpegaudio,ac3,dca,flac,opus,vorbis,vp8,vp9,av1,mpeg4video,mpegvideo,h263,vc1,dvaudio,gsm,mjpeg,png,webp,cook,sipr,xma \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,aac_adtstoasc,vp9_superframe_split \
        --enable-swresample --enable-swscale > config.out 2>&1 ) \
      || { tail -20 "$FFB/config.out"; echo "[ffmpeg] configure не прошёл"; exit 1; }
fi

say "собираю библиотеки FFmpeg (это долго только в первый раз)"
( cd "$FFB" && PATH="$ROOT/src/libs/ffmpeg:$PATH" make -j"$(nproc)" > make.out 2>&1 ) \
  || { grep -m 20 'error' "$FFB/make.out"; echo "[ffmpeg] сборка не прошла"; exit 1; }

# --- 4. прослойка av_* и линковка в .ll ---------------------------------------
say "собираю прослойку av_* (src/libs/ffmpeg/avshim.c)"
$CC -O2 -Wall -Wextra -I"$SRC" -I"$FFB" -c "$ROOT/src/libs/ffmpeg/avshim.c" -o "$OBJ/avshim.o"

# --no-relax обязателен: иначе компоновщик превращает обращения через GOT в
# абсолютные 32-битные (R_X86_64_32S), а библиотека живёт по адресу
# 0x6000_0000_0000, который в 32 бита не влезает.
say "линкую образ библиотеки"
ld -m elf_x86_64 -T "$ROOT/src/libs/lib.ld" -z max-page-size=4096 \
   --emit-relocs -no-pie --no-relax \
   "$OBJ/avshim.o" "$OBJ/ffsys.o" "$OBJ/libc.o" "$OBJ/libm.o" \
   "$FFB/libavformat/libavformat.a" "$FFB/libavcodec/libavcodec.a" \
   "$FFB/libswscale/libswscale.a" "$FFB/libswresample/libswresample.a" \
   "$FFB/libavutil/libavutil.a" "$(gcc -print-libgcc-file-name)" \
   -o "$OBJ/ffmpeg_ll.elf" 2>&1 | grep -v 'RWX' || true
[ -f "$OBJ/ffmpeg_ll.elf" ] || { echo "[ffmpeg] линковка не прошла"; exit 1; }

python3 "$ROOT/tools/elf2ll.py" "$OBJ/ffmpeg_ll.elf" "$OUT"
say "готово: $OUT"
