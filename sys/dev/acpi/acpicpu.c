/* $OpenBSD: acpicpu.c,v 1.6 2006/03/04 05:36:42 marco Exp $ */
/*
 * Copyright (c) 2005 Marco Peereboom <marco@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/malloc.h>

#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

#include <sys/sensors.h>

int	acpicpu_match(struct device *, void *, void *);
void	acpicpu_attach(struct device *, struct device *, void *);
int	acpicpu_notify(struct aml_node *, int, void *);

struct acpicpu_softc {
	struct device		sc_dev;

	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;

	struct acpi_softc	*sc_acpi;
	struct aml_node		*sc_devnode;

	int			sc_pss_len;
	struct acpicpu_pss	*sc_pss;

	struct acpicpu_pct	sc_pct;
};

int	acpicpu_getpct(struct acpicpu_softc *);
int	acpicpu_getpss(struct acpicpu_softc *);

struct cfattach acpicpu_ca = {
	sizeof(struct acpicpu_softc), acpicpu_match, acpicpu_attach
};

struct cfdriver acpicpu_cd = {
	NULL, "acpicpu", DV_DULL
};

int
acpicpu_match(struct device *parent, void *match, void *aux)
{
	struct acpi_attach_args	*aa = aux;
	struct cfdata		*cf = match;
	
	printf("acpicpu_match: %s %s\n", 
		aa->aaa_name, cf->cf_driver->cd_name);

	/* sanity */
	if (aa->aaa_name == NULL ||
	    strcmp(aa->aaa_name, cf->cf_driver->cd_name) != 0 ||
	    aa->aaa_table != NULL)
		return (0);

	return (1);
}

void
acpicpu_attach(struct device *parent, struct device *self, void *aux)
{
	struct acpicpu_softc	*sc = (struct acpicpu_softc *)self;
	struct acpi_attach_args *aa = aux;
	int			i;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_devnode = aa->aaa_node->child;

	sc->sc_pss = NULL;

	printf(": %s: ", sc->sc_devnode->parent->name);
	if (acpicpu_getpss(sc)) {
		/* XXX not the right test but has to do for now */
		printf("can't attach, no _PSS\n");
		return;
	}

#ifdef ACPI_DEBUG
	for (i = 0; i < sc->sc_pss_len; i++) {
		dnprintf(20, "%d %d %d %d %d %d\n",
		    sc->sc_pss[i].pss_core_freq,
		    sc->sc_pss[i].pss_power,
		    sc->sc_pss[i].pss_trans_latency,
		    sc->sc_pss[i].pss_bus_latency,
		    sc->sc_pss[i].pss_ctrl,
		    sc->sc_pss[i].pss_status);
	}
	dnprintf(20, "\n");
#endif
	for (i = 0; i < sc->sc_pss_len; i++)
		printf("%d%s", sc->sc_pss[i].pss_core_freq,
		    i < sc->sc_pss_len - 1 ? ", " : " MHz\n");

	acpicpu_getpct(sc);

	/* aml_register_notify(sc->sc_devnode->parent, aa->aaa_dev, acpicpu_notify, sc); */
}

int
acpicpu_getpct(struct acpicpu_softc *sc)
{
	struct aml_value	res, env;
	struct acpi_context	*ctx;
	char			pb[8];

	memset(&res, 0, sizeof(res));
	memset(&env, 0, sizeof(env));

	ctx = NULL;
	if (aml_eval_name(sc->sc_acpi, sc->sc_devnode, "_PPC", &res, &env)) {
		dnprintf(20, "%s: no _PPC\n", DEVNAME(sc));
		printf("%s: no _PPC\n", DEVNAME(sc));
		return (1);
	}

	dnprintf(10, "_PPC: %d\n", aml_val2int(NULL, &res));

	if (aml_eval_name(sc->sc_acpi, sc->sc_devnode, "_PCT", &res, &env)) {
		dnprintf(20, "%s: no _PCT\n", DEVNAME(sc));
		printf("%s: no _PCT\n", DEVNAME(sc));
		return (1);
	}

	if (res.length != 2) {
		printf("%s: %s: invalid _PCT length\n", DEVNAME(sc),
		    sc->sc_devnode->parent->name);
		return (1);
	}

	memcpy(&sc->sc_pct.pct_ctrl, res.v_package[0]->v_buffer,
	    sizeof sc->sc_pct.pct_ctrl);
	memcpy(&sc->sc_pct.pct_status, res.v_package[1]->v_buffer,
	    sizeof sc->sc_pct.pct_status);

	dnprintf(10, "_PCT(ctrl)  : %02x %04x %02x %02x %02x %02x %016x\n",
	    sc->sc_pct.pct_ctrl.grd_descriptor,
	    sc->sc_pct.pct_ctrl.grd_length,
	    sc->sc_pct.pct_ctrl.grd_gas.address_space_id,
	    sc->sc_pct.pct_ctrl.grd_gas.register_bit_width,
	    sc->sc_pct.pct_ctrl.grd_gas.register_bit_offset,
	    sc->sc_pct.pct_ctrl.grd_gas.access_size,
	    sc->sc_pct.pct_ctrl.grd_gas.address);

	dnprintf(10, "_PCT(status): %02x %04x %02x %02x %02x %02x %016x\n",
	    sc->sc_pct.pct_status.grd_descriptor,
	    sc->sc_pct.pct_status.grd_length,
	    sc->sc_pct.pct_status.grd_gas.address_space_id,
	    sc->sc_pct.pct_status.grd_gas.register_bit_width,
	    sc->sc_pct.pct_status.grd_gas.register_bit_offset,
	    sc->sc_pct.pct_status.grd_gas.access_size,
	    sc->sc_pct.pct_status.grd_gas.address);

	acpi_debug = 111;
	acpi_gasio(sc->sc_acpi, ACPI_IOREAD,
	   sc->sc_pct.pct_ctrl.grd_gas.address_space_id,
	   sc->sc_pct.pct_ctrl.grd_gas.address,
	   1,
	   4,
	   //sc->sc_pct.pct_ctrl.grd_gas.register_bit_width >> 3,
	   pb);

	acpi_gasio(sc->sc_acpi, ACPI_IOWRITE,
	   sc->sc_pct.pct_ctrl.grd_gas.address_space_id,
	   sc->sc_pct.pct_ctrl.grd_gas.address,
	   1,
	   4,
	   //sc->sc_pct.pct_ctrl.grd_gas.register_bit_width >> 3,
	   &sc->sc_pss[3].pss_ctrl);

	acpi_gasio(sc->sc_acpi, ACPI_IOREAD,
	   sc->sc_pct.pct_ctrl.grd_gas.address_space_id,
	   sc->sc_pct.pct_ctrl.grd_gas.address,
	   1,
	   4,
	   //sc->sc_pct.pct_ctrl.grd_gas.register_bit_width >> 3,
	   pb);
	printf("acpicpu: %02x %02x %02x %02x\n", pb[0], pb[1], pb[2], pb[3]);
	acpi_debug = 21;

	return (0);
}

int
acpicpu_getpss(struct acpicpu_softc *sc)
{
	struct aml_value	res, env;
	struct acpi_context	*ctx;
	int			i;

	memset(&res, 0, sizeof(res));
	memset(&env, 0, sizeof(env));

	ctx = NULL;
	if (aml_eval_name(sc->sc_acpi, sc->sc_devnode, "_PSS", &res, &env)) {
		dnprintf(20, "%s: no _PSS\n", DEVNAME(sc));
		return (1);
	}
	
	if (!sc->sc_pss)
		sc->sc_pss = malloc(res.length * sizeof *sc->sc_pss, M_DEVBUF,
		    M_WAITOK);
	memset(sc->sc_pss, 0, res.length * sizeof *sc->sc_pss);

	for (i = 0; i < res.length; i++) {
		sc->sc_pss[i].pss_core_freq = aml_val2int(ctx,
		    res.v_package[i]->v_package[0]);
		sc->sc_pss[i].pss_power = aml_val2int(ctx,
		    res.v_package[i]->v_package[1]);
		sc->sc_pss[i].pss_trans_latency = aml_val2int(ctx,
		    res.v_package[i]->v_package[2]);
		sc->sc_pss[i].pss_bus_latency = aml_val2int(ctx,
		    res.v_package[i]->v_package[3]);
		sc->sc_pss[i].pss_ctrl = aml_val2int(ctx,
		    res.v_package[i]->v_package[4]);
		sc->sc_pss[i].pss_status = aml_val2int(ctx,
		    res.v_package[i]->v_package[5]);
	}

	sc->sc_pss_len = res.length;

	return (0);
}

int
acpicpu_notify(struct aml_node *node, int notify_type, void *arg)
{
	struct acpicpu_softc	*sc = arg;

	dnprintf(10, "acpicpu_notify: %.2x %s\n", notify_type,
	    sc->sc_devnode->parent->name);

	printf("acpicpu_notify: %.2x %s\n", notify_type,
	    sc->sc_devnode->parent->name);

	return (0);
}
