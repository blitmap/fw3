/*
 * firewall3 - 3rd OpenWrt UCI firewall implementation
 *
 *   Copyright (C) 2013 Jo-Philipp Wich <jow@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

#include "zones.h"
#include "ubus.h"


#define C(f, tbl, tgt, name) \
	{ FW3_FAMILY_##f, FW3_TABLE_##tbl, FW3_TARGET_##tgt, name }

struct chain {
	enum fw3_family family;
	enum fw3_table table;
	enum fw3_target target;
	const char *name;
};

static const struct chain src_chains[] = {
	C(ANY, FILTER, UNSPEC,  "zone_%1$s_input"),
	C(ANY, FILTER, UNSPEC,  "zone_%1$s_output"),
	C(ANY, FILTER, UNSPEC,  "zone_%1$s_forward"),

	C(ANY, FILTER, SRC_ACCEPT, "zone_%1$s_src_ACCEPT"),
	C(ANY, FILTER, SRC_REJECT, "zone_%1$s_src_REJECT"),
	C(ANY, FILTER, SRC_DROP,   "zone_%1$s_src_DROP"),
};

static const struct chain dst_chains[] = {
	C(ANY, FILTER, ACCEPT,  "zone_%1$s_dest_ACCEPT"),
	C(ANY, FILTER, REJECT,  "zone_%1$s_dest_REJECT"),
	C(ANY, FILTER, DROP,    "zone_%1$s_dest_DROP"),

	C(V4,  NAT,    SNAT,    "zone_%1$s_postrouting"),
	C(V4,  NAT,    DNAT,    "zone_%1$s_prerouting"),

	C(ANY, FILTER, CUSTOM_CNS_V4, "input_%1$s_rule"),
	C(ANY, FILTER, CUSTOM_CNS_V4, "output_%1$s_rule"),
	C(ANY, FILTER, CUSTOM_CNS_V4, "forwarding_%1$s_rule"),
	C(ANY, FILTER, CUSTOM_CNS_V6, "input_%1$s_rule"),
	C(ANY, FILTER, CUSTOM_CNS_V6, "output_%1$s_rule"),
	C(ANY, FILTER, CUSTOM_CNS_V6, "forwarding_%1$s_rule"),

	C(V4,  NAT,    CUSTOM_CNS_V4, "prerouting_%1$s_rule"),
	C(V4,  NAT,    CUSTOM_CNS_V4, "postrouting_%1$s_rule"),
};


#define R(dir1, dir2) \
	"zone_%1$s_" #dir1 " -m comment --comment \"user chain for %1$s " \
	#dir2 "\" -j " #dir2 "_%1$s_rule"

static const struct chain def_rules[] = {
	C(ANY, FILTER, CUSTOM_CNS_V4, R(input, input)),
	C(ANY, FILTER, CUSTOM_CNS_V4, R(output, output)),
	C(ANY, FILTER, CUSTOM_CNS_V4, R(forward, forwarding)),
	C(ANY, FILTER, CUSTOM_CNS_V6, R(input, input)),
	C(ANY, FILTER, CUSTOM_CNS_V6, R(output, output)),
	C(ANY, FILTER, CUSTOM_CNS_V6, R(forward, forwarding)),

	C(V4,  NAT,    CUSTOM_CNS_V4, R(prerouting, prerouting)),
	C(V4,  NAT,    CUSTOM_CNS_V4, R(postrouting, postrouting)),
};

const struct fw3_option fw3_zone_opts[] = {
	FW3_OPT("enabled",             bool,     zone,     enabled),

	FW3_OPT("name",                string,   zone,     name),
	FW3_OPT("family",              family,   zone,     family),

	FW3_LIST("network",            device,   zone,     networks),
	FW3_LIST("device",             device,   zone,     devices),
	FW3_LIST("subnet",             address,  zone,     subnets),

	FW3_OPT("input",               target,   zone,     policy_input),
	FW3_OPT("forward",             target,   zone,     policy_forward),
	FW3_OPT("output",              target,   zone,     policy_output),

	FW3_OPT("masq",                bool,     zone,     masq),
	FW3_LIST("masq_src",           address,  zone,     masq_src),
	FW3_LIST("masq_dest",          address,  zone,     masq_dest),

	FW3_OPT("extra",               string,   zone,     extra_src),
	FW3_OPT("extra_src",           string,   zone,     extra_src),
	FW3_OPT("extra_dest",          string,   zone,     extra_dest),

	FW3_OPT("conntrack",           bool,     zone,     conntrack),
	FW3_OPT("mtu_fix",             bool,     zone,     mtu_fix),
	FW3_OPT("custom_chains",       bool,     zone,     custom_chains),

	FW3_OPT("log",                 bool,     zone,     log),
	FW3_OPT("log_limit",           limit,    zone,     log_limit),

	{ }
};


static bool
print_chains(enum fw3_table table, enum fw3_family family,
             const char *fmt, const char *name, uint32_t targets,
             const struct chain *chains, int n)
{
	bool rv = false;
	char cn[256] = { 0 };
	const struct chain *c;

	for (c = chains; n > 0; c++, n--)
	{
		if (!fw3_is_family(c, family))
			continue;

		if (c->table != table)
			continue;

		if ((c->target != FW3_TARGET_UNSPEC) && !hasbit(targets, c->target))
			continue;

		snprintf(cn, sizeof(cn), c->name, name);
		fw3_pr(fmt, cn);

		rv = true;
	}

	return rv;
}

static void
check_policy(struct uci_element *e, enum fw3_target *pol, enum fw3_target def,
             const char *name)
{
	if (*pol == FW3_TARGET_UNSPEC)
	{
		warn_elem(e, "has no %s policy specified, using default", name);
		*pol = def;
	}
	else if (*pol > FW3_TARGET_DROP)
	{
		warn_elem(e, "has invalid %s policy, using default", name);
		*pol = def;
	}
}

static void
resolve_networks(struct uci_element *e, struct fw3_zone *zone)
{
	struct fw3_device *net, *tmp;

	list_for_each_entry(net, &zone->networks, list)
	{
		tmp = fw3_ubus_device(net->name);

		if (!tmp)
		{
			warn_elem(e, "cannot resolve device of network '%s'", net->name);
			continue;
		}

		list_add_tail(&tmp->list, &zone->devices);
	}
}

struct fw3_zone *
fw3_alloc_zone(void)
{
	struct fw3_zone *zone;

	zone = malloc(sizeof(*zone));

	if (!zone)
		return NULL;

	memset(zone, 0, sizeof(*zone));

	INIT_LIST_HEAD(&zone->networks);
	INIT_LIST_HEAD(&zone->devices);
	INIT_LIST_HEAD(&zone->subnets);
	INIT_LIST_HEAD(&zone->masq_src);
	INIT_LIST_HEAD(&zone->masq_dest);

	zone->enabled = true;
	zone->custom_chains = true;
	zone->log_limit.rate = 10;

	return zone;
}

void
fw3_load_zones(struct fw3_state *state, struct uci_package *p)
{
	struct uci_section *s;
	struct uci_element *e;
	struct fw3_zone *zone;
	struct fw3_defaults *defs = &state->defaults;

	INIT_LIST_HEAD(&state->zones);

	uci_foreach_element(&p->sections, e)
	{
		s = uci_to_section(e);

		if (strcmp(s->type, "zone"))
			continue;

		zone = fw3_alloc_zone();

		if (!zone)
			continue;

		fw3_parse_options(zone, fw3_zone_opts, s);

		if (!zone->enabled)
		{
			fw3_free_zone(zone);
			continue;
		}

		if (!zone->extra_dest)
			zone->extra_dest = zone->extra_src;

		if (!defs->custom_chains && zone->custom_chains)
			zone->custom_chains = false;

		if (!zone->name || !*zone->name)
		{
			warn_elem(e, "has no name - ignoring");
			fw3_free_zone(zone);
			continue;
		}

		if (list_empty(&zone->networks) && list_empty(&zone->devices) &&
		    list_empty(&zone->subnets) && !zone->extra_src)
		{
			warn_elem(e, "has no device, network, subnet or extra options");
		}

		check_policy(e, &zone->policy_input, defs->policy_input, "input");
		check_policy(e, &zone->policy_output, defs->policy_output, "output");
		check_policy(e, &zone->policy_forward, defs->policy_forward, "forward");

		resolve_networks(e, zone);

		if (zone->masq)
		{
			setbit(zone->flags, FW3_TARGET_SNAT);
			zone->conntrack = true;
		}

		if (zone->custom_chains)
		{
			setbit(zone->flags, FW3_TARGET_SNAT);
			setbit(zone->flags, FW3_TARGET_DNAT);
		}

		setbit(zone->flags, fw3_to_src_target(zone->policy_input));
		setbit(zone->flags, zone->policy_output);
		setbit(zone->flags, zone->policy_forward);

		list_add_tail(&zone->list, &state->zones);
	}
}


static void
print_zone_chain(enum fw3_table table, enum fw3_family family,
                 struct fw3_zone *zone, struct fw3_state *state)
{
	bool s, d, r;
	enum fw3_target f;
	uint32_t custom_mask = ~0;

	if (!fw3_is_family(zone, family))
		return;

	setbit(zone->flags, family);

	/* user chains already loaded, don't create again */
	for (f = FW3_TARGET_CUSTOM_CNS_V4; f <= FW3_TARGET_CUSTOM_CNS_V6; f++)
		if (hasbit(zone->running_flags, f))
			delbit(custom_mask, f);

	if (zone->custom_chains)
		setbit(zone->flags, (family == FW3_FAMILY_V4) ?
		       FW3_TARGET_CUSTOM_CNS_V4 : FW3_TARGET_CUSTOM_CNS_V6);

	if (!zone->conntrack && !state->defaults.drop_invalid)
		setbit(zone->flags, FW3_TARGET_NOTRACK);

	s = print_chains(table, family, ":%s - [0:0]\n", zone->name,
	                 zone->flags,
	                 src_chains, ARRAY_SIZE(src_chains));

	d = print_chains(table, family, ":%s - [0:0]\n", zone->name,
	                 zone->flags & custom_mask,
	                 dst_chains, ARRAY_SIZE(dst_chains));

	r = print_chains(table, family, "-A %s\n", zone->name,
	                 zone->flags,
	                 def_rules, ARRAY_SIZE(def_rules));

	if (s || d || r)
	{
		info("   * Zone '%s'", zone->name);
		fw3_set_running(zone, &state->running_zones);
	}
}

static void
print_interface_rule(enum fw3_table table, enum fw3_family family,
                     struct fw3_zone *zone, struct fw3_device *dev,
                     struct fw3_address *sub, bool disable_notrack)
{
	enum fw3_target t;

#define jump_target(t) \
	((t == FW3_TARGET_REJECT) ? "reject" : fw3_flag_names[t])

	if (table == FW3_TABLE_FILTER)
	{
		for (t = FW3_TARGET_ACCEPT; t <= FW3_TARGET_DROP; t++)
		{
			if (hasbit(zone->flags, fw3_to_src_target(t)))
			{
				fw3_pr("-A zone_%s_src_%s", zone->name, fw3_flag_names[t]);
				fw3_format_in_out(dev, NULL);
				fw3_format_src_dest(sub, NULL);
				fw3_format_extra(zone->extra_src);
				fw3_pr(" -j %s\n", jump_target(t));
			}

			if (hasbit(zone->flags, t))
			{
				fw3_pr("-A zone_%s_dest_%s", zone->name, fw3_flag_names[t]);
				fw3_format_in_out(NULL, dev);
				fw3_format_src_dest(NULL, sub);
				fw3_format_extra(zone->extra_dest);
				fw3_pr(" -j %s\n", jump_target(t));
			}
		}

		fw3_pr("-A delegate_input");
		fw3_format_in_out(dev, NULL);
		fw3_format_src_dest(sub, NULL);
		fw3_format_extra(zone->extra_src);
		fw3_pr(" -j zone_%s_input\n", zone->name);

		fw3_pr("-A delegate_forward");
		fw3_format_in_out(dev, NULL);
		fw3_format_src_dest(sub, NULL);
		fw3_format_extra(zone->extra_src);
		fw3_pr(" -j zone_%s_forward\n", zone->name);

		fw3_pr("-A delegate_output");
		fw3_format_in_out(NULL, dev);
		fw3_format_src_dest(NULL, sub);
		fw3_format_extra(zone->extra_dest);
		fw3_pr(" -j zone_%s_output\n", zone->name);
	}
	else if (table == FW3_TABLE_NAT)
	{
		if (hasbit(zone->flags, FW3_TARGET_DNAT))
		{
			fw3_pr("-A delegate_prerouting");
			fw3_format_in_out(dev, NULL);
			fw3_format_src_dest(sub, NULL);
			fw3_format_extra(zone->extra_src);
			fw3_pr(" -j zone_%s_prerouting\n", zone->name);
		}

		if (hasbit(zone->flags, FW3_TARGET_SNAT))
		{
			fw3_pr("-A delegate_postrouting");
			fw3_format_in_out(NULL, dev);
			fw3_format_src_dest(NULL, sub);
			fw3_format_extra(zone->extra_dest);
			fw3_pr(" -j zone_%s_postrouting\n", zone->name);
		}
	}
	else if (table == FW3_TABLE_MANGLE)
	{
		if (zone->mtu_fix)
		{
			if (zone->log)
			{
				fw3_pr("-A mssfix");
				fw3_format_in_out(NULL, dev);
				fw3_format_src_dest(NULL, sub);
				fw3_pr(" -p tcp --tcp-flags SYN,RST SYN");
				fw3_format_limit(&zone->log_limit);
				fw3_format_comment(zone->name, " (mtu_fix logging)");
				fw3_pr(" -j LOG --log-prefix \"MSSFIX(%s): \"\n", zone->name);
			}

			fw3_pr("-A mssfix");
			fw3_format_in_out(NULL, dev);
			fw3_format_src_dest(NULL, sub);
			fw3_pr(" -p tcp --tcp-flags SYN,RST SYN");
			fw3_format_comment(zone->name, " (mtu_fix)");
			fw3_pr(" -j TCPMSS --clamp-mss-to-pmtu\n");
		}
	}
	else if (table == FW3_TABLE_RAW)
	{
		if (!zone->conntrack && !disable_notrack)
		{
			fw3_pr("-A notrack");
			fw3_format_in_out(dev, NULL);
			fw3_format_src_dest(sub, NULL);
			fw3_format_extra(zone->extra_src);
			fw3_format_comment(zone->name, " (notrack)");
			fw3_pr(" -j CT --notrack\n", zone->name);
		}
	}
}

static void
print_interface_rules(enum fw3_table table, enum fw3_family family,
                      struct fw3_zone *zone, bool disable_notrack)
{
	struct fw3_device *dev;
	struct fw3_address *sub;

	fw3_foreach(dev, &zone->devices)
	fw3_foreach(sub, &zone->subnets)
	{
		if (!fw3_is_family(sub, family))
			continue;

		if (!dev && !sub)
			continue;

		print_interface_rule(table, family, zone, dev, sub, disable_notrack);
	}
}

static void
print_zone_rule(enum fw3_table table, enum fw3_family family,
                struct fw3_zone *zone, bool disable_notrack)
{
	struct fw3_address *msrc;
	struct fw3_address *mdest;

	enum fw3_target t;

	if (!fw3_is_family(zone, family))
		return;

	switch (table)
	{
	case FW3_TABLE_FILTER:
		fw3_pr("-A zone_%s_input -j zone_%s_src_%s\n",
			   zone->name, zone->name, fw3_flag_names[zone->policy_input]);

		fw3_pr("-A zone_%s_forward -j zone_%s_dest_%s\n",
			   zone->name, zone->name, fw3_flag_names[zone->policy_forward]);

		fw3_pr("-A zone_%s_output -j zone_%s_dest_%s\n",
			   zone->name, zone->name, fw3_flag_names[zone->policy_output]);

		if (zone->log)
		{
			for (t = FW3_TARGET_REJECT; t <= FW3_TARGET_DROP; t++)
			{
				if (hasbit(zone->flags, fw3_to_src_target(t)))
				{
					fw3_pr("-A zone_%s_src_%s", zone->name, fw3_flag_names[t]);
					fw3_format_limit(&zone->log_limit);
					fw3_pr(" -j LOG --log-prefix \"%s(src %s)\"\n",
						   fw3_flag_names[t], zone->name);
				}

				if (hasbit(zone->flags, t))
				{
					fw3_pr("-A zone_%s_dest_%s", zone->name, fw3_flag_names[t]);
					fw3_format_limit(&zone->log_limit);
					fw3_pr(" -j LOG --log-prefix \"%s(dest %s)\"\n",
						   fw3_flag_names[t], zone->name);
				}
			}
		}
		break;

	case FW3_TABLE_NAT:
		if (zone->masq && family == FW3_FAMILY_V4)
		{
			fw3_foreach(msrc, &zone->masq_src)
			fw3_foreach(mdest, &zone->masq_dest)
			{
				fw3_pr("-A zone_%s_postrouting ", zone->name);
				fw3_format_src_dest(msrc, mdest);
				fw3_pr("-j MASQUERADE\n");
			}
		}
		break;

	case FW3_TABLE_RAW:
	case FW3_TABLE_MANGLE:
		break;
	}

	print_interface_rules(table, family, zone, disable_notrack);
}

void
fw3_print_zone_chains(enum fw3_table table, enum fw3_family family,
                      struct fw3_state *state)
{
	struct fw3_zone *zone;

	list_for_each_entry(zone, &state->zones, list)
		print_zone_chain(table, family, zone, state);
}

void
fw3_print_zone_rules(enum fw3_table table, enum fw3_family family,
                     struct fw3_state *state)
{
	struct fw3_zone *zone;

	list_for_each_entry(zone, &state->zones, list)
		print_zone_rule(table, family, zone, state->defaults.drop_invalid);
}

void
fw3_flush_zones(enum fw3_table table, enum fw3_family family,
			    bool pass2, bool reload, struct fw3_state *state)
{
	struct fw3_zone *z, *tmp;
	uint32_t custom_mask = ~0;
	uint32_t family_mask = (1 << FW3_FAMILY_V4) | (1 << FW3_FAMILY_V6);

	/* don't touch user chains on selective stop */
	if (reload)
	{
		delbit(custom_mask, FW3_TARGET_CUSTOM_CNS_V4);
		delbit(custom_mask, FW3_TARGET_CUSTOM_CNS_V6);
	}

	list_for_each_entry_safe(z, tmp, &state->running_zones, running_list)
	{
		if (!hasbit(z->flags, family))
			continue;

		print_chains(table, family, pass2 ? "-X %s\n" : "-F %s\n",
		             z->name, z->running_flags,
		             src_chains, ARRAY_SIZE(src_chains));

		print_chains(table, family, pass2 ? "-X %s\n" : "-F %s\n",
		             z->name, z->running_flags & custom_mask,
		             dst_chains, ARRAY_SIZE(dst_chains));

		if (pass2)
		{
			delbit(z->flags, family);

			if (!(z->flags & family_mask))
				fw3_set_running(z, NULL);
		}
	}
}

struct fw3_zone *
fw3_lookup_zone(struct fw3_state *state, const char *name, bool running)
{
	struct fw3_zone *z;

	if (list_empty(&state->zones))
		return NULL;

	list_for_each_entry(z, &state->zones, list)
	{
		if (strcmp(z->name, name))
			continue;

		if (!running || z->running_list.next)
			return z;

		break;
	}

	return NULL;
}
