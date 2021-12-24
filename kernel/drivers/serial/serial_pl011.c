/*
 * copyright (c) 2018 min le (lemin9538@gmail.com)
 *
 * this program is free software; you can redistribute it and/or modify
 * it under the terms of the gnu general public license version 2 as
 * published by the free software foundation.
 *
 * this program is distributed in the hope that it will be useful,
 * but without any warranty; without even the implied warranty of
 * merchantability or fitness for a particular purpose.  see the
 * gnu general public license for more details.
 *
 * you should have received a copy of the gnu general public license
 * along with this program.  if not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/errno.h>
#include <asm/io.h>
#include <minos/init.h>
#include <config/config.h>
#include "pl011.h"
#include <minos/console.h>
#include <minos/irq.h>

static void *base;

static int __pl011_init(void *addr, int clock, int baudrate)
{
	unsigned int temp;
	unsigned int divider;
	unsigned int remainder;
	unsigned int fraction;

	base = (void *)ptov(addr);

	temp = 16 * baudrate;
	divider = clock / temp;
	remainder = clock % temp;
	temp = (8 * remainder) / baudrate;
	fraction = (temp >> 1) + (temp & 1);

	iowrite32(0x0, base + UARTCR);
	iowrite32(0x0, base + UARTECR);
	iowrite32(0x0 | PL011_LCR_WORD_LENGTH_8 | \
		  PL011_LCR_ONE_STOP_BIT | \
		  PL011_LCR_PARITY_DISABLE | \
		  PL011_LCR_BREAK_DISABLE, base + UARTLCR_H);

	iowrite32(divider, base + UARTIBRD);
	iowrite32(fraction, base + UARTFBRD);

	iowrite32(INT_RX, base + UARTIMSC);	// enable rx interrupt.

	iowrite32(PL011_ICR_CLR_ALL_IRQS, base + UARTICR);
	iowrite32(0x0 | PL011_CR_UART_ENABLE | \
		  PL011_CR_TX_ENABLE | \
		  PL011_CR_RX_ENABLE, base + UARTCR);

	return 0;
}

static int pl011_irq_handler(uint32_t irq, void *data)
{
	unsigned int int_status;

	int_status = ioread32(base + UARTMIS);
	if (int_status & INT_RX) {
		iowrite32(INT_RX, base + UARTICR);
		console_char_recv(ioread32(base + UARTDR) & 0xff);
		iowrite32(0, base + UARTECR);
	}

	return 0;
}

static int pl011_irq_init(void)
{
	return request_irq(CONFIG_UART_IRQ, pl011_irq_handler, 0, "pl011", NULL);
}
device_initcall(pl011_irq_init);

static int pl011_init(char *arg)
{
	return __pl011_init((void *)ptov(CONFIG_UART_BASE), 24000000, 115200);
}

static void serial_pl011_putc(char c)
{
	while (ioread32(base + UARTFR) & PL011_FR_BUSY_FLAG);

	if (c == '\n')
		iowrite32('\r', base + UARTDR);

	iowrite32(c, base + UARTDR);
}

static char serial_pl011_getc(void)
{
	while (ioread32(base + UARTFR) & PL011_FR_BUSY_FLAG);

	return ioread32(base + UARTDR);
}

DEFINE_CONSOLE(pl011, "pl011", pl011_init,
		serial_pl011_putc, serial_pl011_getc);
