
# FAT镜像文件名称
IMAGE_NAME_FAT := fs.fat

# FAT镜像文件名称
IMAGE_NAME_YAFFS2 := fs.yaffs2

IMAGE_NAME_YAFFS2_DATA := data.yaffs2

# 镜像大小 (例如: 4096 = 4兆字节)
IMAGE_SIZE := 16384

# 需要拷贝到镜像中的文件/目录
SOURCE_FILES := ./application

# FAT临时挂载点
MOUNT_POINT_FAT := ./application/img_fat

# FAT临时挂载点
MOUNT_POINT_YAFFS2 := ./application/img_yaffs2

MOUNT_POINT_DATA := ./application/data

ifeq ($(CONFIG_SOC_T32), y)
ALG_PATH := $(SOURCE_FILES)/../third_party/alg_ivs/t32/
else
ALG_PATH := $(SOURCE_FILES)/../third_party/alg_ivs/g32/
endif

ifeq ($(CONFIG_APPLICATION_IVS_FACE), y)
FACE_MODEL_PATH = $(ALG_PATH)/face/model
LIVENESS_MODEL_PATH = $(ALG_PATH)/faceLivess/model
endif

ifeq ($(CONFIG_APPLICATION_MULTI_OBJ_DET), y)
MULTIOBJ_MODEL_PATH = $(ALG_PATH)/multi_obj_det/model
endif

ifeq ($(CONFIG_APPLICATION_SCANCODE), y)
SCANCODE_MODEL_PATH = $(ALG_PATH)/scancode/model
endif

# 资源拷贝函数
# 参数: $(1) = MOUNT_POINT
define copy_files_to_image
	@echo "拷贝文件到镜像..."; \
	if [ "$(CONFIG_APPLICATION_HOME)" = "y" ]; then \
	mkdir -p $(1)/home; \
	sudo cp -r $(SOURCE_FILES)/app_home/res/* $(1)/home/; \
	fi; \
	if [ "$(CONFIG_APPLICATION_IVS_FACE)" = "y" ]; then \
		mkdir -p $(1)/face_model; \
		sudo cp -r $(FACE_MODEL_PATH)/* $(1)/face_model; \
		sudo cp -r $(LIVENESS_MODEL_PATH)/* $(1)/face_model/; \
		mkdir -p $(1)/video; \
		sudo cp -r $(SOURCE_FILES)/app_ivs_face/res/* $(1)/video/; \
	fi; \
	if [ "$(CONFIG_APPLICATION_MULTI_OBJ_DET)" = "y" ]; then \
		mkdir -p $(1)/multi_obj_model; \
		sudo cp -r $(MULTIOBJ_MODEL_PATH)/* $(1)/multi_obj_model; \
	fi; \
	if [ "$(CONFIG_APPLICATION_SCANCODE)" = "y" ]; then \
		mkdir -p $(1)/scancode_model; \
		sudo cp -r $(SCANCODE_MODEL_PATH)/* $(1)/scancode_model; \
	fi; \
	if [ "$(CONFIG_APPLICATION_STATUS_BAR)" = "y" ]; then \
		mkdir -p $(1)/status_bar; \
		sudo cp -r $(SOURCE_FILES)/app_status_bar/res/* $(1)/status_bar/; \
	fi; \
	if [ "$(CONFIG_APPLICATION_SETTING)" = "y" ]; then \
		mkdir -p $(1)/setting; \
		sudo cp -r $(SOURCE_FILES)/app_setting/res/* $(1)/setting/; \
	fi; \
	if [ "$(CONFIG_APPLICATION_MOTOR_DISPLAY)" = "y" ]; then \
		mkdir -p $(1)/setting; \
		sudo cp -r $(SOURCE_FILES)/app_setting/res/* $(1)/setting/; \
	fi; \
	if [ "$(CONFIG_APPLICATION_AUDIO)" = "y" ]; then \
		mkdir -p $(1)/audio; \
		sudo cp -r $(SOURCE_FILES)/app_audio/res/* $(1)/audio/; \
		mkdir -p $(1)/kws_model; \
		sudo cp -r $(ALG_PATH)/kws/model/* $(1)/kws_model; \
	fi; \
	if [ "$(CONFIG_APPLICATION_TIRTC_DEMO)" = "y" ]; then \
		mkdir -p $(1)/tirtc; \
		sudo cp -r $(SOURCE_FILES)/app_tirtc_demo/res/* $(1)/tirtc/; \
	fi; \
	if [ "$(CONFIG_APPLICATION_ALIYUN)" = "y" ]; then \
		mkdir -p $(1)/aliyun; \
		sudo cp $(SOURCE_FILES)/app_aliyun/res/config.json $(1)/aliyun/config.json; \
	fi; \
	if [ "$(CONFIG_APPLICATION_MICROPYTHON)" = "y" ]; then \
		mkdir -p $(1)/micropython; \
		sudo cp -r $(SOURCE_FILES)/app_micropython/res/micropython/* $(1)/micropython/; \
	fi
endef

# 创建FAT镜像并拷贝文件
fat:
	$(if $(CONFIG_APPLICATION_SETTING),@bash application/app_setting/build/dev_configured.sh)
	@echo "使用mkfs.fat创建镜像..."
	mkfs.fat -S 4096 -C $(IMAGE_NAME_FAT) $(IMAGE_SIZE)
	@echo "创建临时挂载点..."
	mkdir -p $(MOUNT_POINT_FAT)

	@echo "挂载镜像文件..."
	sudo mount -o loop $(IMAGE_NAME_FAT) $(MOUNT_POINT_FAT)

	$(call copy_files_to_image,$(MOUNT_POINT_FAT))

	@echo "卸载镜像..."
	sudo umount $(MOUNT_POINT_FAT)
	rmdir $(MOUNT_POINT_FAT)
	@echo "镜像构建完成: $(IMAGE_NAME_FAT)"

fat_clean:
	@if [ -f "$(IMAGE_NAME_FAT)" ]; then \
		rm -f $(IMAGE_NAME_FAT); \
		echo "已删除镜像文件: $(IMAGE_NAME_FAT)"; \
	else \
		echo "镜像文件不存在"; \
	fi
	@if [ -d "$(MOUNT_POINT_FAT)" ]; then \
		sudo umount $(MOUNT_POINT_FAT) 2>/dev/null; \
		rmdir $(MOUNT_POINT_FAT); \
	fi


# 创建YAFFS2镜像并拷贝文件
yaffs2:
	$(if $(CONFIG_APPLICATION_SETTING),@bash application/app_setting/build/dev_configured.sh)
	@echo "使用filesystem/output_tools/mkyaffs2创建镜像..."
	mkdir -p $(MOUNT_POINT_YAFFS2)

	$(call copy_files_to_image,$(MOUNT_POINT_YAFFS2))

	sudo ./filesystem/output_tools/mkyaffs2  -i -p 2048 -s 64 $(MOUNT_POINT_YAFFS2) $(IMAGE_NAME_YAFFS2)

	@echo "镜像构建完成: $(IMAGE_NAME_YAFFS2)"

yaffs2_clean:
	@if [ -f "$(IMAGE_NAME_YAFFS2)" ]; then \
		rm -f $(IMAGE_NAME_YAFFS2); \
		echo "已删除镜像文件: $(IMAGE_NAME_YAFFS2)"; \
	else \
		echo "镜像文件不存在"; \
	fi
	@if [ -d "$(MOUNT_POINT_YAFFS2)" ]; then \
		sudo rm -rf $(MOUNT_POINT_YAFFS2); \
	fi

yaffs2_data:
	@echo "使用filesystem/output_tools/mkyaffs2创建data镜像..."
	mkdir -p $(MOUNT_POINT_DATA)

	sudo ./filesystem/output_tools/mkyaffs2  -i -p 2048 -s 64 $(MOUNT_POINT_DATA) $(IMAGE_NAME_YAFFS2_DATA)

	@echo "镜像构建完成: $(IMAGE_NAME_YAFFS2_DATA)"

yaffs2_data_clean:
	@if [ -f "$(IMAGE_NAME_DATA)" ]; then \
		rm -f $(IMAGE_NAME_YAFFS2_DATA); \
		echo "已删除镜像文件: $(IMAGE_NAME_YAFFS2_DATA)"; \
	else \
		echo "镜像文件不存在"; \
	fi
	@if [ -d "$(MOUNT_POINT_DATA)" ]; then \
		sudo rm -rf $(MOUNT_POINT_DATA); \
	fi
