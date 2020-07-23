/*
 * MIT License
 *
 * Copyright (c) 2020 Newracom, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */


#include "hif.h"
#include "hal_uart.h"

/*********************************************************************************************/

/*
 *	FIFO depth -> TX: 16x8, RX: 16x12
 * 	FIFO level -> 0:1/8, 1:1/4, 2:1/2, 3:3/4, 4:7/8
 */
#define _HIF_UART_RX_HW_FIFO_LEVEL		4
#define _HIF_UART_TX_HW_FIFO_LEVEL		0

/*********************************************************************************************/

#define _HIF_UART_REG_BASE				g_hif_uart_reg_base[g_hif_uart.channel]

#define _hif_uart_rx_int_enable()		RegHSUART_IMSC(_HIF_UART_REG_BASE) |= (IMSC_RX|IMSC_RT)
#define _hif_uart_rx_int_disable()		RegHSUART_IMSC(_HIF_UART_REG_BASE) &= ~(IMSC_RX|IMSC_RT)

#define _hif_uart_tx_int_enable()		RegHSUART_IMSC(_HIF_UART_REG_BASE) |= IMSC_TX
#define _hif_uart_tx_int_disable()		RegHSUART_IMSC(_HIF_UART_REG_BASE) &= ~IMSC_TX

#define _hif_uart_int_get_status()		RegHSUART_MIS(_HIF_UART_REG_BASE)
#define _hif_uart_int_set_status(mask)	RegHSUART_MIS(_HIF_UART_REG_BASE) |= mask

#define _hif_uart_int_clear(mask)		RegHSUART_ICR(_HIF_UART_REG_BASE) |= mask

#define _hif_uart_rx_data()				(RegHSUART_DR(_HIF_UART_REG_BASE) & 0xff)
#define _hif_uart_rx_full()				(RegHSUART_FR(_HIF_UART_REG_BASE) & FR_RXFF)
#define _hif_uart_rx_empty()			(RegHSUART_FR(_HIF_UART_REG_BASE) & FR_RXFE)

#define _hif_uart_tx_data(data)			RegHSUART_DR(_HIF_UART_REG_BASE) = data
#define _hif_uart_tx_full()				(RegHSUART_FR(_HIF_UART_REG_BASE) & FR_TXFF)
#define _hif_uart_tx_empty()			(RegHSUART_FR(_HIF_UART_REG_BASE) & FR_TXFE)

/*********************************************************************************************/

const uint32_t g_hif_uart_reg_base[4] =
{
	HSUART0_BASE_ADDR, HSUART1_BASE_ADDR, HSUART2_BASE_ADDR, HSUART3_BASE_ADDR
};

/*********************************************************************************************/

static _hif_uart_t g_hif_uart =
{
	.channel = -1,
	.baudrate = -1,
	.hfc = false
};

static void _hif_uart_init_info (_hif_uart_t *info)
{
	info->channel = -1;
	info->baudrate = -1;
	info->data_bits = UART_DB8;
	info->stop_bits = UART_SB1;
	info->parity = UART_PB_NONE;
	info->hfc = UART_HFC_DISABLE;
}

static void _hif_uart_set_info (_hif_uart_t *info)
{
	if (info)
		memcpy(&g_hif_uart, info, sizeof(_hif_uart_t));
}

void _hif_uart_get_info (_hif_uart_t *info)
{
	if (info)
		memcpy(info, &g_hif_uart, sizeof(_hif_uart_t));
}

/*********************************************************************************************/

static _hif_fifo_t *g_hif_uart_rx_fifo = NULL;
static _hif_fifo_t *g_hif_uart_tx_fifo = NULL;

static void _hif_uart_fifo_delete (void)
{
	if (g_hif_uart_rx_fifo)
	{
		_hif_fifo_delete(g_hif_uart_rx_fifo);
		g_hif_uart_rx_fifo = NULL;
	}

	if (g_hif_uart_tx_fifo)
	{
		_hif_fifo_delete(g_hif_uart_tx_fifo);
		g_hif_uart_tx_fifo = NULL;
	}
}

static int _hif_uart_fifo_create (_hif_info_t *info)
{
	_hif_buf_t *rx_fifo = &info->rx_fifo;
	_hif_buf_t *tx_fifo = &info->tx_fifo;

	if (rx_fifo->size > 0)
		g_hif_uart_rx_fifo = _hif_fifo_create(rx_fifo->addr, rx_fifo->size, true);

	if (tx_fifo->size > 0)
		g_hif_uart_tx_fifo = _hif_fifo_create(tx_fifo->addr, tx_fifo->size, false);

#ifdef CONFIG_HIF_UART_TX_POLLING
	if (!g_hif_uart_rx_fifo || g_hif_uart_tx_fifo)
#else
	if (!g_hif_uart_rx_fifo && !g_hif_uart_tx_fifo)
#endif
	{
		_hif_error("_hif_fifo_create() failed, rx=%p(%d) tx=%p(%d)\n",
			   			g_hif_uart_rx_fifo, rx_fifo->size,
						g_hif_uart_tx_fifo, tx_fifo->size);

		_hif_uart_fifo_delete();
		return -1;
	}

	_hif_debug("UART FIFO: rx=%p, tx=%p\n", g_hif_uart_rx_fifo, g_hif_uart_tx_fifo);
	_hif_info("UART FIFO: rx=%u, tx=%u\n",
				g_hif_uart_rx_fifo ? g_hif_uart_rx_fifo->size : 0,
				g_hif_uart_tx_fifo ? g_hif_uart_tx_fifo->size : 0);

	return 0;
}

/*********************************************************************************************/

bool _hif_uart_channel_valid (int channel)
{
	switch (channel)
	{
#ifndef CONFIG_HIF_UART_CH2_ONLY
		case 0:
		case 3:
#endif
		case 2:
			return true;
	}

	return false;
}

bool _hif_uart_baudrate_valid (int baudrate, int hfc)
{
	switch (baudrate)
	{
		case 19200:
		case 38400:
			return true;

		case 57600:
		case 115200:
		case 230400:
		case 380400:
		case 460800:
		case 500000:
		case 576000:
		case 921600:
		case 1000000:
		case 1152000:
		case 1500000:
		case 2000000:
			if (hfc == UART_HFC_ENABLE)
				return true;
	}

	return false;
}

static int _hif_uart_channel_to_vector (int channel)
{
	const int vector[4] = {EV_HSUART0, EV_HSUART1, EV_HSUART2, EV_HSUART3};

	return vector[channel];
}

/*********************************************************************************************/

int _hif_uart_getc (char *data)
{
	if (g_hif_uart.channel < 0)
		return -1;

	if (g_hif_uart_rx_fifo)
	{
		_hif_fifo_mutex_take(g_hif_uart_rx_fifo);

		if (_hif_fifo_read(g_hif_uart_rx_fifo, data, 1) != 1)
		{
			_hif_fifo_mutex_give(g_hif_uart_rx_fifo);
			return -1;
		}

		_hif_fifo_mutex_give(g_hif_uart_rx_fifo);
	}
	else
	{
		if (_hif_uart_rx_empty())
			return -1;

		*data = _hif_uart_rx_data();
	}

	return 0;
}

int _hif_uart_putc (char data)
{
	if (g_hif_uart.channel < 0)
		return -1;

	if (g_hif_uart_tx_fifo)
	{
/*		_hif_fifo_mutex_take(g_hif_uart_tx_fifo); */

		if (_hif_fifo_write(g_hif_uart_tx_fifo, &data, 1) != 1)
		{
/*			_hif_fifo_mutex_give(g_hif_uart_tx_fifo); */
			return -1;
		}

/*		_hif_fifo_mutex_give(g_hif_uart_tx_fifo); */
	}
	else
	{
		if (_hif_uart_tx_full())
			return -1;

		_hif_uart_tx_data(data);
	}

	return 0;
}

int _hif_uart_read (char *buf, int len)
{
	int rx_cnt = 0;

	if (g_hif_uart.channel < 0)
		return 0;

	if (g_hif_uart_rx_fifo)
	{
		_hif_uart_rx_int_disable();

		if (_hif_fifo_mutex_take(g_hif_uart_rx_fifo))
		{
			rx_cnt = _hif_fifo_read(g_hif_uart_rx_fifo, buf, len);

			_hif_fifo_mutex_give(g_hif_uart_rx_fifo);
		}

		_hif_uart_rx_int_enable();
	}
	else
	{
		for (rx_cnt = 0 ; rx_cnt < len ; rx_cnt++)
		{
			if (_hif_uart_rx_empty())
				break;

			buf[rx_cnt] = _hif_uart_rx_data();
		}
	}

	return rx_cnt;
}

int _hif_uart_write (char *buf, int len)
{
	int tx_cnt = 0;

	if (g_hif_uart.channel < 0)
		return 0;

	if (g_hif_uart_tx_fifo)
	{
/*		_hif_fifo_mutex_take(g_hif_uart_tx_fifo); */

		tx_cnt = _hif_fifo_write(g_hif_uart_tx_fifo, buf, len);

/*		_hif_fifo_mutex_give(g_hif_uart_tx_fifo); */

		if (tx_cnt > 0)
			_hif_uart_tx_int_enable();
	}
	else
	{
		if (_hif_uart_tx_empty())
		{
			for (tx_cnt = 0 ; tx_cnt < 16 && tx_cnt < len ; tx_cnt++)
			{
				if (_hif_uart_tx_full())
				{
/*					_hif_debug("uart_tx_full, cnt=%d\n", tx_cnt); */
					break;
				}

				_hif_uart_tx_data(buf[tx_cnt]);
			}
		}
	}

	return tx_cnt;
}

/*********************************************************************************************/

static void _hif_uart_rx_isr (void)
{
	if (_hif_fifo_mutex_take_isr(g_hif_uart_rx_fifo))
	{
		int fifo_size;
		int i;

		fifo_size = _hif_fifo_free_size(g_hif_uart_rx_fifo);
		if (fifo_size == 0)
		{
/*			_hif_error("fifo_full\n"); */

			_hif_uart_rx_int_disable();
		}
		else
		{
			for (i = 0 ; i < fifo_size ; i++)
			{
				if (_hif_uart_rx_empty())
					break;

				_hif_fifo_putc(g_hif_uart_rx_fifo, _hif_uart_rx_data());
			}
		}

		_hif_rx_resume_isr();
		_hif_fifo_mutex_give_isr(g_hif_uart_rx_fifo);
	}
}

#ifndef CONFIG_HIF_UART_TX_POLLING
static void _hif_uart_tx_isr (void)
{
	static char buf[16 + 1];
	static unsigned int size = 0;
	static unsigned int cnt = 0;
	int i;

	if (g_hif_uart_tx_fifo)
	{
/*		if (!_hif_fifo_mutex_take_isr(g_hif_uart_tx_fifo))
			return; */

		if (cnt >= size)
		{
			cnt = 0;
			size = _hif_fifo_read(g_hif_uart_tx_fifo, buf, sizeof(buf));
			if (size == 0)
			{
				_hif_uart_tx_int_disable();
				return;
			}
		}

/*		_hif_fifo_mutex_give_isr(g_hif_uart_tx_fifo); */

		for (i = 0 ; i < (size - cnt) ; i++)
		{
			if (_hif_uart_tx_full())
				break;

			_hif_uart_tx_data(buf[cnt + i]);
		}

		cnt += i;

		_hif_uart_tx_int_enable();
	}
}
#endif

static void _hif_uart_isr (int vector)
{
/*	unsigned long flags = system_irq_save(); */

	if (g_hif_uart.channel > 0)
	{
		static volatile uint32_t status;

		status = _hif_uart_int_get_status();

/*		_hif_debug("uart_int: 0x%08X\n", status); */

		_hif_uart_int_clear(status & (ICR_RT|ICR_RX|ICR_TX));

		if (status & (MIS_RX|MIS_RT))
			_hif_uart_rx_isr();

#ifndef CONFIG_HIF_UART_TX_POLLING
		if (status & MIS_TX)
			_hif_uart_tx_isr();
#endif
	}

/*	system_irq_restore(flags); */
}

/*********************************************************************************************/

#define _hif_uart_pin_info(fmt, ...)		//_hif_info(fmt, ##__VA_ARGS__)

static void _hif_uart_pin_enable (int channel)
{
	gpio_io_t gpio;
	uio_sel_t uio;

	/*
     * UART0: TX=GP4 RX=GP5
	 * UART2: TX=GP0 RX=GP1 RTS=GP2 CTS=GP3
	 * UART3: TX=GP6 RX=GP7
	 */

	/* GPIO_ATL0 */
	nrc_gpio_get_alt(&gpio);
	_hif_uart_pin_info("GPIO_ALT0: 0x%X\n", gpio.word);

	switch (channel)
	{
		case 0:
			gpio.bit.io4 = 1;
			gpio.bit.io5 = 1;
			break;

		case 2:
			gpio.bit.io0 = 1;
			gpio.bit.io1 = 1;
			gpio.bit.io2 = 1;
			gpio.bit.io3 = 1;
			break;

		case 3:
			gpio.bit.io6 = 1;
			gpio.bit.io7 = 1;
	}

	nrc_gpio_set_alt(&gpio);

	nrc_gpio_get_alt(&gpio);
	_hif_uart_pin_info("GPIO_ALT0: 0x%X\n", gpio.word);

	/* UIO_SEL */
	switch (channel)
	{
		case 0:
			nrc_gpio_get_uio_sel(UIO_SEL_UART0, &uio);
			_hif_uart_pin_info("UIO_SEL_UART0: 0x%X\n", uio.word);

			uio.bit.sel7_0 = 4;
			uio.bit.sel15_8 = 5;
			uio.bit.sel23_16 = 0xff;
			uio.bit.sel31_24 = 0xff;

			nrc_gpio_set_uio_sel(UIO_SEL_UART0, &uio);

			nrc_gpio_get_uio_sel(UIO_SEL_UART0, &uio);
			_hif_uart_pin_info("UIO_SEL_UART0: 0x%X\n", uio.word);
			break;

		case 2:
			nrc_gpio_get_uio_sel(UIO_SEL_UART2, &uio);
			_hif_uart_pin_info("UIO_SEL_UART2: 0x%X\n", uio.word);

			uio.bit.sel7_0 = 0;
			uio.bit.sel15_8 = 1;
			uio.bit.sel23_16 = 2;
			uio.bit.sel31_24 = 3;

			nrc_gpio_set_uio_sel(UIO_SEL_UART2, &uio);

			nrc_gpio_get_uio_sel(UIO_SEL_UART2, &uio);
			_hif_uart_pin_info("UIO_SEL_UART2: 0x%X\n", uio.word);
			break;

		case 3:
			nrc_gpio_get_uio_sel(UIO_SEL_UART3, &uio);
			_hif_uart_pin_info("UIO_SEL_UART3: 0x%X\n", uio.word);

			uio.bit.sel7_0 = 6;
			uio.bit.sel15_8 = 7;
			uio.bit.sel23_16 = 0xff;
			uio.bit.sel31_24 = 0xff;

			nrc_gpio_set_uio_sel(UIO_SEL_UART3, &uio);

			nrc_gpio_get_uio_sel(UIO_SEL_UART3, &uio);
			_hif_uart_pin_info("UIO_SEL_UART3: 0x%X\n", uio.word);
	}

	/* GPIO_DIR */
	nrc_gpio_get_dir(&gpio);
	_hif_uart_pin_info("GPIO DIR: 0x%08X\n", gpio.word);

	gpio.word &= ~(1 << 9);
	nrc_gpio_config_dir(&gpio);

	nrc_gpio_get_dir(&gpio);
	_hif_uart_pin_info("GPIO DIR: 0x%08X\n", gpio.word);

	/* GPIO_PU */
	nrc_gpio_get_pullup(&gpio);
	_hif_uart_pin_info("GPIO PU: 0x%08X\n", gpio.word);

	/* GPIO_PD */
	nrc_gpio_get_pulldown(&gpio);
	_hif_uart_pin_info("GPIO PD: 0x%08X\n", gpio.word);
}

static void _hif_uart_pin_disable (int channel)
{
	gpio_io_t gpio;
	uio_sel_t uio;

	/*
     * UART0: TX=GP4 RX=GP5
	 * UART2: TX=GP0 RX=GP1 RTS=GP2 CTS=GP3
	 * UART3: TX=GP6 RX=GP7
	 */

	/* GPIO_ATL0 */
	nrc_gpio_get_alt(&gpio);
	_hif_uart_pin_info("GPIO_ALT0: 0x%X\n", gpio.word);

	switch (channel)
	{
		case 0:
			gpio.bit.io4 = 0;
			gpio.bit.io5 = 0;
			break;

		case 2:
			gpio.bit.io0 = 0;
			gpio.bit.io1 = 0;
			gpio.bit.io2 = 0;
			gpio.bit.io3 = 0;
			break;

		case 3:
			gpio.bit.io6 = 0;
			gpio.bit.io7 = 0;
	}

	nrc_gpio_set_alt(&gpio);
	nrc_gpio_get_alt(&gpio);
	_hif_uart_pin_info("GPIO_ALT0: 0x%X\n", gpio.word);

	/* UIO_SEL */
	switch (channel)
	{
		case 0:
			nrc_gpio_get_uio_sel(UIO_SEL_UART0, &uio);
			_hif_uart_pin_info("UIO_SEL_UART0: 0x%X\n", uio.word);

			uio.bit.sel7_0 = 0xff;
			uio.bit.sel15_8 = 0xff;
			uio.bit.sel23_16 = 0xff;
			uio.bit.sel31_24 = 0xff;

			nrc_gpio_set_uio_sel(UIO_SEL_UART0, &uio);
			nrc_gpio_get_uio_sel(UIO_SEL_UART0, &uio);
			_hif_uart_pin_info("UIO_SEL_UART0: 0x%X\n", uio.word);
			break;

		case 2:
			nrc_gpio_get_uio_sel(UIO_SEL_UART2, &uio);
			_hif_uart_pin_info("UIO_SEL_UART2: 0x%X\n", uio.word);

			uio.bit.sel7_0 = 0xff;
			uio.bit.sel15_8 = 0xff;
			uio.bit.sel23_16 = 0xff;
			uio.bit.sel31_24 = 0xff;

			nrc_gpio_set_uio_sel(UIO_SEL_UART2, &uio);
			nrc_gpio_get_uio_sel(UIO_SEL_UART2, &uio);
			_hif_uart_pin_info("UIO_SEL_UART2: 0x%X\n", uio.word);
			break;

		case 3:
			nrc_gpio_get_uio_sel(UIO_SEL_UART3, &uio);
			_hif_uart_pin_info("UIO_SEL_UART3: 0x%X\n", uio.word);

			uio.bit.sel7_0 = 0xff;
			uio.bit.sel15_8 = 0xff;
			uio.bit.sel23_16 = 0xff;
			uio.bit.sel31_24 = 0xff;

			nrc_gpio_set_uio_sel(UIO_SEL_UART3, &uio);
			nrc_gpio_get_uio_sel(UIO_SEL_UART3, &uio);
			_hif_uart_pin_info("UIO_SEL_UART3: 0x%X\n", uio.word);
	}
}

static int _hif_uart_enable (_hif_uart_t *uart)
{
	const int non_hfc_baudrate_max = CONFIG_HIF_UART_BAUDRATE;

	if (!uart || !_hif_uart_channel_valid(uart->channel))
		return -1;

	if (uart->channel != 2)
	{
		if (uart->hfc)
		{
			_hif_info("UART Enable: Channel %d can not use hardware flow control.\n",
							uart->channel);
			return -1;
		}

		if (uart->baudrate > non_hfc_baudrate_max)
		{
			_hif_info("UART Enable: Channel %d can not use baudrate greater than %dbps.\n",
							uart->channel, non_hfc_baudrate_max);
			return -1;
		}
	}
	else if (!uart->hfc && uart->baudrate > non_hfc_baudrate_max)
	{
		_hif_info("UART Enable: baudrate is greater than %dbps\n", non_hfc_baudrate_max);
		return -1;
	}

	_hif_info("UART Enable: channel=%d badurate=%d data=%d stop=%d parity=%s hfc=%s\r\n",
			uart->channel, uart->baudrate,
			uart->data_bits + 5, uart->stop_bits + 1,
			uart->parity == 0 ? "none" : (uart->parity == 1 ? "odd" : "even"),
			uart->hfc ? "on" : "off");

	nrc_hsuart_init(uart->channel, uart->data_bits, uart->baudrate, uart->stop_bits,
			uart->parity, uart->hfc, UART_FIFO_ENABLE);

	nrc_hsuart_fifo_level(uart->channel, _HIF_UART_TX_HW_FIFO_LEVEL, _HIF_UART_RX_HW_FIFO_LEVEL);

	nrc_hsuart_int_clr(uart->channel, true, true, true); /* tx_empty, rx_done, rx_timeout */
	nrc_hsuart_interrupt(uart->channel, false, true); 	 /* tx_empty, rx_done & rx_timeout */

	system_register_isr(_hif_uart_channel_to_vector(uart->channel), _hif_uart_isr);
	system_irq_unmask(_hif_uart_channel_to_vector(uart->channel));

	_hif_uart_pin_enable(uart->channel);

	_hif_uart_set_info(uart);

	return 0;
}

static void _hif_uart_disable (void)
{
	if (_hif_uart_channel_valid(g_hif_uart.channel))
	{
		int channel = g_hif_uart.channel;

		_hif_info("UART Disable: channel=%d\r\n", channel);

		_hif_uart_pin_disable(channel);

		nrc_hsuart_interrupt(channel, false, false); 	/* tx_en, rx_rt_en */
		nrc_hsuart_int_clr(channel, true, true, true); 	/* tx_int, rx_int, rt_int */

/*		system_irq_mask(_hif_uart_channel_to_vector(channel)); */
		system_register_isr(_hif_uart_channel_to_vector(channel), NULL);

		_hif_uart_init_info(&g_hif_uart);
	}
}

/*********************************************************************************************/

int _hif_uart_open (_hif_info_t *info)
{
	if (info && info->type == _HIF_TYPE_UART)
	{
		_hif_uart_t *uart = &info->uart;

		_hif_info("UART Open: channel=%d baudrate=%d hfc=%s\n",
					uart->channel, uart->baudrate, uart->hfc ? "on" : "off");

		if (_hif_uart_channel_valid(uart->channel) &&
			_hif_uart_baudrate_valid(uart->baudrate, uart->hfc))
		{
			if (_hif_uart_fifo_create(info) == 0)
			{
				if (_hif_uart_enable(uart) == 0)
				{
					_hif_info("UART Open: success\n");
					return 0;
				}
			}
		}
	}

	_hif_info("UART Open: fail\n");

	return -1;
}

void _hif_uart_close (void)
{
	if (_hif_uart_channel_valid(g_hif_uart.channel))
	{
/*		_hif_info("UART Close: channel=%d", g_hif_uart.channel); */

		_hif_uart_disable();

		_hif_uart_fifo_delete();

		_hif_uart_init_info(&g_hif_uart);

/*		_hif_info("UART Close: success\n"); */
	}

	_hif_info("UART Close: no open\n");
}

int _hif_uart_change (_hif_uart_t *new)
{
	_hif_uart_t old;

	if (new)
	{
		if (!_hif_uart_channel_valid(new->channel))
			_hif_info("UART Change: invalid channel=%d\n", new->channel);
		else
		{
			_hif_uart_get_info(&old);

			_hif_info("UART Change: \n");
			_hif_info(" - channel: %d->%d\n", old.channel, new->channel);
			_hif_info(" - baudrate: %d->%d\n", old.baudrate, new->baudrate);
			_hif_info(" - data bits: %d->%d\n", old.data_bits, new->data_bits);
			_hif_info(" - stop  bits: %d->%d\n", old.stop_bits, new->stop_bits);
			_hif_info(" - parity: %d->%d\n", old.parity, new->parity);
			_hif_info(" - hfc: %d->%d\n", old.hfc, new->hfc);

			if (new->channel != 2 && new->hfc == UART_HFC_ENABLE)
				_hif_info("UART Change: channel %d can not use hardware flow control.\n", new->channel);
			else
			{
				_hif_uart_disable();

				if (_hif_uart_enable(new) == 0)
				{
					_hif_info("UART Change: success\n");

					return 0;
				}

				_hif_uart_enable(&old);
			}
		}
	}

	_hif_info("UART Change: fail\n");

	return -1;
}
