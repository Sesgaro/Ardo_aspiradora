################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../source/Aspiradora.c \
../source/BSPInit.c \
../source/I2C.c \
../source/INA3221.c \
../source/LEDBlink.c \
../source/at_manager.c \
../source/isr.c \
../source/mtb.c \
../source/semihost_hardfault.c \
../source/ultrasonico.c 

C_DEPS += \
./source/Aspiradora.d \
./source/BSPInit.d \
./source/I2C.d \
./source/INA3221.d \
./source/LEDBlink.d \
./source/at_manager.d \
./source/isr.d \
./source/mtb.d \
./source/semihost_hardfault.d \
./source/ultrasonico.d 

OBJS += \
./source/Aspiradora.o \
./source/BSPInit.o \
./source/I2C.o \
./source/INA3221.o \
./source/LEDBlink.o \
./source/at_manager.o \
./source/isr.o \
./source/mtb.o \
./source/semihost_hardfault.o \
./source/ultrasonico.o 


# Each subdirectory must supply rules for building sources it contributes
source/%.o: ../source/%.c source/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: MCU C Compiler'
	arm-none-eabi-gcc -D__REDLIB__ -DCPU_MKL25Z128VLK4 -DCPU_MKL25Z128VLK4_cm0plus -DSDK_OS_BAREMETAL -DFSL_RTOS_BM -DSDK_DEBUGCONSOLE=1 -DCR_INTEGER_PRINTF -DPRINTF_FLOAT_ENABLE=0 -D__MCUXPRESSO -D__USE_CMSIS -DDEBUG -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora/board" -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora/source" -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora" -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora/drivers" -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora/startup" -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora/utilities" -I"/home/sesgaro/Ardo_aspiradora/kl25z_codes/Aspiradora/CMSIS" -O0 -fno-common -g3 -gdwarf-4 -Wall -c -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections -fmerge-constants -fmacro-prefix-map="$(<D)/"= -mcpu=cortex-m0plus -mthumb -D__REDLIB__ -fstack-usage -specs=redlib.specs -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.o)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-source

clean-source:
	-$(RM) ./source/Aspiradora.d ./source/Aspiradora.o ./source/BSPInit.d ./source/BSPInit.o ./source/I2C.d ./source/I2C.o ./source/INA3221.d ./source/INA3221.o ./source/LEDBlink.d ./source/LEDBlink.o ./source/at_manager.d ./source/at_manager.o ./source/isr.d ./source/isr.o ./source/mtb.d ./source/mtb.o ./source/semihost_hardfault.d ./source/semihost_hardfault.o ./source/ultrasonico.d ./source/ultrasonico.o

.PHONY: clean-source

