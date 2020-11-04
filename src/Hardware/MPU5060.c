#include "MPU5060.h"
#include "driver_init.h"
#include "utils.h"

void I2C_write_bits(struct i2c_m_sync_desc *const i2c, uint8_t reg, uint8_t mask, uint8_t val) {
	uint8_t buf[2] = {reg, 0};
	i2c_m_sync_cmd_read(i2c, reg, &buf[1], 1);
	for (volatile int i = 0; i < 500; ++i);

	buf[1] &= mask;
	buf[1] |= val;

	i2c_m_sync_write_reg(i2c, buf[0], buf[1]);
	for (volatile int i = 0; i < 800; ++i);
}

uint8_t I2C_read_reg(struct i2c_m_sync_desc *const i2c, uint8_t reg) {
	uint8_t read;
	i2c_m_sync_cmd_read(i2c, reg, &read, 1);
	for (volatile int i = 0; i < 500; ++i);

	return read;
}

void MPU5060_initialize(void) {
	struct io_descriptor *I2C_0_io;
	
	i2c_m_sync_get_io_descriptor(&I2C_0, &I2C_0_io);
	i2c_m_sync_enable(&I2C_0);
	i2c_m_sync_set_slaveaddr(&I2C_0, 0x68, I2C_M_SEVEN);

	I2C_read_reg(&I2C_0, 0x75);

	I2C_write_bits(&I2C_0, 0x6B, 0xF8, 0x01);			//	PWR_MGMT_1
	I2C_write_bits(&I2C_0, 0x1B, 0xF0, 0x08);			//	GYRO_CONFIG
	I2C_write_bits(&I2C_0, 0x6B, 0xBF, 0x00);			//	PWR_MGMT_1

	I2C_read_reg(&I2C_0, 0x6B);
}

void MPU5060_read(uint8_t *buf) {
	struct io_descriptor *I2C_0_io;
	uint8_t rd_buf[6];

	i2c_m_sync_get_io_descriptor(&I2C_0, &I2C_0_io);
	i2c_m_sync_enable(&I2C_0);
	i2c_m_sync_set_slaveaddr(&I2C_0, 0x68, I2C_M_SEVEN);

	i2c_m_sync_cmd_read(&I2C_0, 0x43, rd_buf, 6);	//	GYRO OUT
	for (volatile int i = 0; i < 1500; ++i);

	buf[0] = rd_buf[1];		//	X
	buf[1] = rd_buf[0];

	buf[2] = rd_buf[3];		//	Y
	buf[3] = rd_buf[2];

	buf[4] = rd_buf[5];		//	Z
	buf[5] = rd_buf[4];
}

