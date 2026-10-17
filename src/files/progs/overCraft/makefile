# overCraft: воксельная игра (ovcraft.prg) и её движок (ovengine.ll) из одной папки
TARGET      := ovcraft.prg
SRCS        := main.cpp world/generator.cpp world/mesher.cpp world/world.cpp \
               game/player.cpp game/physics.cpp game/hud.cpp game/icons.cpp game/inventory.cpp \
               game/model.cpp game/shapes.cpp game/entity.cpp game/hand.cpp
USE_LIBUI   := 1
LDSCRIPT    := overcraft.ld
APP_NAME    := overCraft
APP_ID      := ovcraft
LL_TARGET   := ovengine.ll
LL_SRCS     := engine/api.cpp engine/renderer.cpp engine/jobs.cpp
LL_ARENA_KB := 65536
include ../../../libs/common/app.mk
