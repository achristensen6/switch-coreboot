/*
 * (C) Copyright David Gibson <dwg@au1.ibm.com>, IBM Corporation.  2005.
 *
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *                                                                       
 *  You should have received a copy of the GNU General Public License    
 *  along with this program; if not, write to the Free Software          
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 
 *                                                                   USA 
 */

#include "dtc.h"
#include "flat_dt.h"

#define FTF_FULLPATH	0x1
#define FTF_VARALIGN	0x2
#define FTF_NAMEPROPS	0x4
#define FTF_BOOTCPUID	0x8
#define FTF_STRTABSIZE	0x10

static struct version_info {
	int version;
	int last_comp_version;
	int hdr_size;
	int flags;
} version_table[] = {
	{1, 1, BPH_V1_SIZE,
	 FTF_FULLPATH|FTF_VARALIGN|FTF_NAMEPROPS},
	{2, 1, BPH_V2_SIZE,
	 FTF_FULLPATH|FTF_VARALIGN|FTF_NAMEPROPS|FTF_BOOTCPUID},
	{3, 1, BPH_V3_SIZE,
	 FTF_FULLPATH|FTF_VARALIGN|FTF_NAMEPROPS|FTF_BOOTCPUID|FTF_STRTABSIZE},
	{0x10, 0x10, BPH_V3_SIZE,
	 FTF_BOOTCPUID|FTF_STRTABSIZE},
};

struct emitter {
	void (*cell)(void *, cell_t);
	void (*string)(void *, char *, int);
	void (*align)(void *, int);
	void (*data)(void *, struct property *);
	void (*beginnode)(void *, char *);
	void (*endnode)(void *, char *);
	void (*property)(void *, char *);
	void(*special)(FILE *f, struct node *);
};

char *
clean(char *name, int instantiate){
	char *cleanname = strdup(name), *cp;

	for(cp = cleanname; *cp; cp++)
		switch(*cp) {
			case '#': 
				*cp = 'H';
				break;
			case '@':
				/* if we are declaring it, this is an instance. If not, this is a struct type */
				if (instantiate)
					*cp = '_'; 
				else
					*cp = 0;
				break;
			case '-':
			case ',':
				*cp = '_';
				break;
		}

	return cleanname;
}

char *
topath(struct property *p){
	struct data d = p->val;
	int i = 0;
	char *pathname, *cp;

	pathname = malloc(d.len + 1);

	for(cp = pathname, i = 0; i < d.len; i++, cp++) {
		switch(d.val[i]){
			case '@':
				*cp = 0;
				break;
			case ',':
				*cp = '/';
				break;
			default: 
				*cp = d.val[i];
				break;
		}
	}
	*cp++ = 0;
	return pathname;
}



static void bin_emit_cell(void *e, cell_t val)
{
	struct data *dtbuf = e;

	*dtbuf = data_append_cell(*dtbuf, val);
}

static void bin_emit_string(void *e, char *str, int len)
{
	struct data *dtbuf = e;

	if (len == 0)
		len = strlen(str);

	*dtbuf = data_append_data(*dtbuf, str, len);
	*dtbuf = data_append_byte(*dtbuf, '\0');
}

static void bin_emit_align(void *e, int a)
{
	struct data *dtbuf = e;

	*dtbuf = data_append_align(*dtbuf, a);
}

static void bin_emit_data(void *e, struct property *p)
{
	struct data d = p->val;
	struct data *dtbuf = e;

	*dtbuf = data_append_data(*dtbuf, d.val, d.len);
}

static void bin_emit_beginnode(void *e, char *label)
{
	bin_emit_cell(e, OF_DT_BEGIN_NODE);
}

static void bin_emit_endnode(void *e, char *label)
{
	bin_emit_cell(e, OF_DT_END_NODE);
}

static void bin_emit_property(void *e, char *label)
{
	bin_emit_cell(e, OF_DT_PROP);
}

static struct emitter bin_emitter = {
	.cell = bin_emit_cell,
	.string = bin_emit_string,
	.align = bin_emit_align,
	.data = bin_emit_data,
	.beginnode = bin_emit_beginnode,
	.endnode = bin_emit_endnode,
	.property = bin_emit_property,	
};

static void emit_label(FILE *f, char *prefix, char *label)
{
	fprintf(f, "\t.globl\t%s_%s\n", prefix, label);
	fprintf(f, "%s_%s:\n", prefix, label);
	fprintf(f, "_%s_%s:\n", prefix, label);
}

static void asm_emit_cell(void *e, cell_t val)
{
	FILE *f = e;

	fprintf(f, "\t.long\t0x%x\n", val);
}

static void asm_emit_string(void *e, char *str, int len)
{
	FILE *f = e;
	char c;

	if (len != 0) {
		/* XXX: ewww */
		c = str[len];
		str[len] = '\0';
	}
		
	fprintf(f, "\t.string\t\"%s\"\n", str);

	if (len != 0) {
		str[len] = c;
	}
}

static void asm_emit_align(void *e, int a)
{
	FILE *f = e;

	fprintf(f, "\t.balign\t%d\n", a);
}

static void asm_emit_data(void *e, struct property *p)
{
	struct data d = p->val;
	FILE *f = e;
	int off = 0;

	while ((d.len - off) >= sizeof(u32)) {
		fprintf(f, "\t.long\t0x%x\n",
			be32_to_cpu(*((u32 *)(d.val+off))));
		off += sizeof(u32);
	}

	if ((d.len - off) >= sizeof(u16)) {
		fprintf(f, "\t.short\t0x%hx\n", 
			be16_to_cpu(*((u16 *)(d.val+off))));
		off += sizeof(u16);
	}

	if ((d.len - off) >= 1) {
		fprintf(f, "\t.byte\t0x%hhx\n", d.val[off]);
		off += 1;
	}

	assert(off == d.len);
}

static void asm_emit_beginnode(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "\t.globl\t%s\n", label);
		fprintf(f, "%s:\n", label);
	}
	fprintf(f, "\t.long\tOF_DT_BEGIN_NODE\n");
}

static void asm_emit_endnode(void *e, char *label)
{
	FILE *f = e;

	fprintf(f, "\t.long\tOF_DT_END_NODE\n");
	if (label) {
		fprintf(f, "\t.globl\t%s_end\n", label);
		fprintf(f, "%s_end:\n", label);
	}
}

static void asm_emit_property(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "\t.globl\t%s\n", label);
		fprintf(f, "%s:\n", label);
	}
	fprintf(f, "\t.long\tOF_DT_PROP\n");
}

static struct emitter asm_emitter = {
	.cell = asm_emit_cell,
	.string = asm_emit_string,
	.align = asm_emit_align,
	.data = asm_emit_data,
	.beginnode = asm_emit_beginnode,
	.endnode = asm_emit_endnode,
	.property = asm_emit_property,	
};

int unique = 0;
static void C_emit_cell(void *e, cell_t val)
{
	FILE *f = e;

	fprintf(f, "\tu32\tc%d = 0x%x;\n", unique++, val);
}

static void C_emit_string(void *e, char *str, int len)
{
	FILE *f = e;
	char c;

	if (len != 0) {
		/* XXX: ewww */
		c = str[len];
		str[len] = '\0';
	}
		
	fprintf(f, "\tunsigned char *c%d  = \t\"%s\";\n", unique++, str);

	if (len != 0) {
		str[len] = c;
	}
}

static void C_emit_align(void *e, int a)
{
	FILE *f = e;

	fprintf(f, "\tALIGN(x)\t%d\n", a);
}

static void C_emit_data(void *e, struct property *p)
{
	struct data d = p->val;
	FILE *f = e;
	int off = 0;

	while ((d.len - off) >= sizeof(u32)) {
		fprintf(f, "\tu32 d%d = \t0x%x;\n",unique++, 
			be32_to_cpu(*((u32 *)(d.val+off))));
		off += sizeof(u32);
	}

	if ((d.len - off) >= sizeof(u16)) {
		fprintf(f, "\tu16 d%d = \t0x%hx;\n", unique++, 
			be16_to_cpu(*((u16 *)(d.val+off))));
		off += sizeof(u16);
	}

	if ((d.len - off) >= 1) {
		fprintf(f, "\tu8 d%d = \t0x%hhx;\n", unique++, d.val[off]);
		off += 1;
	}

	assert(off == d.len);
}

static void C_emit_beginnode(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "struct\t%s {\n", label);
	}
	fprintf(f, "\tu32\tn%d = OF_DT_BEGIN_NODE;\n", unique++);
}

static void C_emit_endnode(void *e, char *label)
{
	FILE *f = e;

	fprintf(f, "\tu32\ten%d = OF_DT_END_NODE;\n", unique++);
	if (label) {
		fprintf(f, "\t\t%s;\n", label);
	}
}

static void C_emit_struct_start(void *e, char *prefix, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "struct\t%s {\n", label);
	}
}

static void C_emit_struct_end(void *e, char *prefix, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "}; /*%s*/\n", label);
	}
}



static void C_emit_property(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "\tstruct p%d\t%s{\n", unique++, label);
	}
	fprintf(f, "\tu32 p%d = \tOF_DT_PROP;\n", unique++);
}

static struct emitter C_emitter = {
	.cell = C_emit_cell,
	.string = C_emit_string,
	.align = C_emit_align,
	.data = C_emit_data,
	.beginnode = C_emit_beginnode,
	.endnode = C_emit_endnode,
	.property = C_emit_property,	
};


/* LinuxBIOS static.c */

static void linuxbios_emit_cell(void *e, cell_t val)
{
	FILE *f = e;

	fprintf(f, "\tu32\tc%d = 0x%x;\n", unique++, val);
}

static void linuxbios_emit_string(void *e, char *str, int len)
{
	FILE *f = e;
	char c;

	if (len != 0) {
		/* XXX: ewww */
		c = str[len];
		str[len] = '\0';
	}
		
	fprintf(f, "\tunsigned char *c%d  = \t\"%s\";\n", unique++, str);

	if (len != 0) {
		str[len] = c;
	}
}

static void linuxbios_emit_align(void *e, int a)
{
	FILE *f = e;

	fprintf(f, "\tALIGN(x)\t%d\n", a);
}

static void linuxbios_emit_data(void *e, struct property *p)
{
	struct data d = p->val;
	FILE *f = e;
	int i;
	char *cleanname;

	/* nothing to do? */
	if (d.len == 0)
		return;

	cleanname = clean(p->name, 1);
	fprintf(f, "\t.%s = {", cleanname);
	free(cleanname);

	for(i = 0; i < d.len; i++)
		fprintf(f, "0x%02x,", d.val[i]);

	fprintf(f, "},\n");
}

static void linuxbios_emit_beginnode(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "struct\t%s {\n", label);
	}
}

static void linuxbios_emit_endnode(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "}; /*%s*/\n", label);
	}
}

#ifdef LINUXBIOS_OUTPUT
static void linuxbios_emit_struct_start(void *e, char *prefix, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "struct\t%s {\n", label);
	}
}

static void linuxbios_emit_struct_end(void *e, char *prefix, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "}; /*%s*/\n", label);
	}
}
#endif


static void linuxbios_emit_property(void *e, char *label)
{
	FILE *f = e;

	if (label) {
		fprintf(f, "\tstruct p%d\t%s{\n", unique++, label);
	}
	fprintf(f, "\tu32 p%d = \tOF_DT_PROP;\n", unique++);
}

static void linuxbios_emit_special(FILE *e, struct node *tree)
{
	FILE *f = e;
	struct property *prop;
	int ops_set = 0;
	int is_root = 0;

	fprintf(f, "struct device dev_%s = {\n", tree->label);
	/* special case -- the root has a distinguished path */
	if (! strncmp(tree->label, "root", 4)){
		is_root = 1;
		fprintf(f, "\t.path =  { .type = DEVICE_PATH_ROOT },\n");
	}

	for_each_property(tree, prop) {
		if (streq(prop->name, "pcidomain")){
			fprintf(f, "\t.path = {.type=DEVICE_PATH_PCI_DOMAIN,.u={.pci_domain={ .domain = %s }}},\n", 
				prop->val.val);
		}
		if (streq(prop->name, "pcipath")){
			fprintf(f, "\t.path = {.type=DEVICE_PATH_PCI,.u={.pci={ .devfn = PCI_DEVFN(%s)}}},\n", 
				prop->val.val);
		}
		/* to do: check the value, maybe. Kinda pointless though. */
		if (streq(prop->name, "on_mainboard")){
			fprintf(f, "\t.on_mainboard = 1,\n");
		}
		if (streq(prop->name, "enabled")){
			fprintf(f, "\t.enabled = 1,\n");
		}

		if (streq(prop->name, "config")){
			fprintf(f, "\t.chip_ops = &%s_ops,\n", clean(prop->val.val, 0));
			fprintf(f, "\t.chip_info = &%s,\n", clean(tree->label, 1));
		}

		if (streq(prop->name, "ops")){
			fprintf(f, "\t.ops = &%s,\n", clean(prop->val.val, 0));
			ops_set  = 1;
		}

		if (streq(prop->name, "ops_pci")){
			fprintf(f, "\t.ops_pci = &%s,\n", clean(prop->val.val, 0));
			ops_set  = 1;
		}

		if (streq(prop->name, "ops_pci_bus")){
			fprintf(f, "\t.ops_pci_bus = &%s,\n", clean(prop->val.val, 0));
			ops_set  = 1;
		}

		if (streq(prop->name, "ops_smbus_bus")){
			fprintf(f, "\t.ops_smbus_bus = &%s,\n", clean(prop->val.val, 0));
			ops_set  = 1;
		}
	}
	if (tree->next_sibling) 
		fprintf(f, "\t.sibling = &dev_%s,\n", tree->next_sibling->label);
	if (tree->next) 
		fprintf(f, "\t.next = &dev_%s,\n", tree->next->label);
	/* now do we do next? */
	/* this will need to do a bus for every child. And, below, we're going to need to find which bus we're on*/
	/* for now, let's keep it to the minimum that will work, while we see if we like this. */
	if (tree->children){
		fprintf(f,"\t.links = 1,\n");
		fprintf(f,"\t.link = {\n");
		fprintf(f,"\t\t[0] = {\n");
		fprintf(f,"\t\t\t.dev = &dev_%s,\n", tree->label);
		fprintf(f,"\t\t\t.link = 0,\n");
		fprintf(f,"\t\t\t.children = &dev_%s\n", tree->children->label);
		fprintf(f,"\t\t},\n");
		fprintf(f,"\t},\n");
	}
	/* fill in the 'bus I am on' entry */
	if (tree->parent)
		fprintf(f, "\t.bus = &dev_%s.link[0],\n", tree->parent->label);
	else
		fprintf(f, "\t.bus = &dev_%s.link[0],\n", tree->label);

	if (tree->next)
		fprintf(f, "\t.next = &dev_%s,\n", tree->next->label);

	if ((! ops_set) && is_root)
		fprintf(f, "\t.ops = &default_dev_ops_root,\n");

	fprintf(f, "\t.dtsname = \"%s\",\n", tree->label);

	fprintf(f, "};\n");
}

static struct emitter linuxbios_emitter = {
	.cell = linuxbios_emit_cell,
	.string = linuxbios_emit_string,
	.align = linuxbios_emit_align,
	.data = linuxbios_emit_data,
	.beginnode = linuxbios_emit_beginnode,
	.endnode = linuxbios_emit_endnode,
	.property = linuxbios_emit_property,
	.special = linuxbios_emit_special,	
};


static int stringtable_insert(struct data *d, char *str)
{
	int i;

	/* FIXME: do this more efficiently? */

	for (i = 0; i < d->len; i++) {
		if (streq(str, d->val + i))
			return i;
	}

	*d = data_append_data(*d, str, strlen(str)+1);
	return i;
}

/* we're going to overload the name node for testing. This may be the wrong thing long-term */
static void flatten_tree_emit_includes(struct node *tree, struct emitter *emit,
			 void *etarget, struct data *strbuf,
			 struct version_info *vi)
{
	char *pathname;
	struct property *prop;
	struct node *child;
	FILE *f = etarget;

	for_each_property(tree, prop) {
		if (streq(prop->name, "config")) {
			pathname = topath(prop);
			fprintf(f, "#include <%s/config.h>\n", pathname);
			free(pathname);
		}
	}

	for_each_child(tree, child) {
		flatten_tree_emit_includes(child, emit, etarget, strbuf, vi);
	}


}

	
static void flatten_tree_emit_structdecls(struct node *tree, struct emitter *emit,
			 void *etarget, struct data *strbuf,
			 struct version_info *vi)
{
	char *treename;

	struct property *prop;
	struct node *child;
	int seen_name_prop = 0;
	FILE *f = etarget;

	treename = clean(tree->name, 0);
	emit->beginnode(etarget, treename);


#if 0
	if (vi->flags & FTF_FULLPATH)
		emit->string(etarget, tree->fullpath, 0);
	else
		emit->string(etarget, tree->name, 0);
#endif

	for_each_property(tree, prop) {
		char *cleanname;
		if (streq(prop->name, "name"))
			seen_name_prop = 1;
		cleanname = clean(prop->name, 0);
		fprintf(f, "\tu8 %s[%d];\n", cleanname, prop->val.len);
		free(cleanname);

	}
#if 0
	if ((vi->flags & FTF_NAMEPROPS) && !seen_name_prop) {
		fprintf(f, "\tu8 %s[%d];\n", prop->name, prop->val.len);
	}
#endif
	emit->endnode(etarget, treename);
	free(treename);

	for_each_child(tree, child) {
		flatten_tree_emit_structdecls(child, emit, etarget, strbuf, vi);
	}


}

int structunique  = 0;

static void flatten_tree_emit_structinits(struct node *tree, struct emitter *emit,
			 void *etarget, struct data *strbuf,
			 struct version_info *vi)
{
	char *treename;

	struct property *prop;
	struct node *child;
	int seen_name_prop = 0;
	FILE *f = etarget;
/*
	treename = clean(tree->name, 0);
	fprintf(f, "struct %s %s = {\n", treename, tree->label);
	free(treename);

*/
#if 0
	if (vi->flags & FTF_FULLPATH)
		emit->string(etarget, tree->fullpath, 0);
	else
		emit->string(etarget, tree->name, 0);
#endif
/*
	for_each_property(tree, prop) {
		if (streq(prop->name, "name"))
			seen_name_prop = 1;
		emit->data(etarget, prop);
	}
 */
#if 0
	if ((vi->flags & FTF_NAMEPROPS) && !seen_name_prop) {
		fprintf(f, "\tu8 %s[%d]\n", prop->name, prop->data.len);
	}
#endif
/*
	emit->endnode(etarget, tree->label);
*/

	/* now emit the device for this node, with sibling and child pointers etc. */
	emit->special(f, tree);

	for_each_child(tree, child) {
		flatten_tree_emit_structinits(child, emit, etarget, strbuf, vi);
	}


}


static void flatten_tree(struct node *tree, struct emitter *emit,
			 void *etarget, struct data *strbuf,
			 struct version_info *vi)
{
	struct property *prop;
	struct node *child;
	int seen_name_prop = 0;

	emit->beginnode(etarget, tree->label);

	if (vi->flags & FTF_FULLPATH)
		emit->string(etarget, tree->fullpath, 0);
	else
		emit->string(etarget, tree->name, 0);

	emit->align(etarget, sizeof(cell_t));

	for_each_property(tree, prop) {
		int nameoff;

		if (streq(prop->name, "name"))
			seen_name_prop = 1;

		nameoff = stringtable_insert(strbuf, prop->name);

		emit->property(etarget, prop->label);
		emit->cell(etarget, prop->val.len);
		emit->cell(etarget, nameoff);

		if ((vi->flags & FTF_VARALIGN) && (prop->val.len >= 8))
			emit->align(etarget, 8);

		emit->data(etarget, prop);
		emit->align(etarget, sizeof(cell_t));
	}

	if ((vi->flags & FTF_NAMEPROPS) && !seen_name_prop) {
		emit->property(etarget, NULL);
		emit->cell(etarget, tree->basenamelen+1);
		emit->cell(etarget, stringtable_insert(strbuf, "name"));

		if ((vi->flags & FTF_VARALIGN) && ((tree->basenamelen+1) >= 8))
			emit->align(etarget, 8);

		emit->string(etarget, tree->name, tree->basenamelen);
		emit->align(etarget, sizeof(cell_t));
	}

	for_each_child(tree, child) {
		flatten_tree(child, emit, etarget, strbuf, vi);
	}

	emit->endnode(etarget, tree->label);
}

static struct data flatten_reserve_list(struct reserve_info *reservelist,
				 struct version_info *vi)
{
	struct reserve_info *re;
	struct data d = empty_data;

	for (re = reservelist; re; re = re->next) {
		d = data_append_re(d, &re->re);
	}
	
	return d;
}
static void make_bph(struct boot_param_header *bph,
		     struct version_info *vi,
		     int reservesize, int dtsize, int strsize,
		     int boot_cpuid_phys)
{
	int reserve_off;

	reservesize += sizeof(struct reserve_entry);

	memset(bph, 0xff, sizeof(*bph));

	bph->magic = cpu_to_be32(OF_DT_HEADER);
	bph->version = cpu_to_be32(vi->version);
	bph->last_comp_version = cpu_to_be32(vi->last_comp_version);

	/* Reserve map should be doubleword aligned */
	reserve_off = ALIGN(vi->hdr_size, 8);

	bph->off_mem_rsvmap = cpu_to_be32(reserve_off);
	bph->off_dt_struct = cpu_to_be32(reserve_off + reservesize);
	bph->off_dt_strings = cpu_to_be32(reserve_off + reservesize
					  + dtsize);
	bph->totalsize = cpu_to_be32(reserve_off + reservesize
				     + dtsize + strsize);
		
	if (vi->flags & FTF_BOOTCPUID)
		bph->boot_cpuid_phys = cpu_to_be32(boot_cpuid_phys);
	if (vi->flags & FTF_STRTABSIZE)
		bph->size_dt_strings = cpu_to_be32(strsize);
}

void dt_to_blob(FILE *f, struct boot_info *bi, int version,
		int boot_cpuid_phys)
{
	struct version_info *vi = NULL;
	int i;
	struct data dtbuf = empty_data;
	struct data strbuf = empty_data;
	struct data reservebuf;
	struct boot_param_header bph;
	struct reserve_entry termre = {.address = 0, .size = 0};

	for (i = 0; i < ARRAY_SIZE(version_table); i++) {
		if (version_table[i].version == version)
			vi = &version_table[i];
	}
	if (!vi)
		die("Unknown device tree blob version %d\n", version);

	dtbuf = empty_data;
	strbuf = empty_data;

	flatten_tree(bi->dt, &bin_emitter, &dtbuf, &strbuf, vi);
	bin_emit_cell(&dtbuf, OF_DT_END);

	reservebuf = flatten_reserve_list(bi->reservelist, vi);

	/* Make header */
	make_bph(&bph, vi, reservebuf.len, dtbuf.len, strbuf.len,
		 boot_cpuid_phys);

	fwrite(&bph, vi->hdr_size, 1, f);

	/* Align the reserve map to an 8 byte boundary */
	for (i = vi->hdr_size; i < be32_to_cpu(bph.off_mem_rsvmap); i++)
		fputc(0, f);

	/*
	 * Reserve map entries.
	 * Each entry is an (address, size) pair of u64 values.
	 * Always supply a zero-sized temination entry.
	 */
	fwrite(reservebuf.val, reservebuf.len, 1, f);
	fwrite(&termre, sizeof(termre), 1, f);

	fwrite(dtbuf.val, dtbuf.len, 1, f);
	fwrite(strbuf.val, strbuf.len, 1, f);

	if (ferror(f))
		die("Error writing device tree blob: %s\n", strerror(errno));

	data_free(dtbuf);
	data_free(strbuf);
}

static void dump_stringtable_asm(FILE *f, struct data strbuf)
{
	unsigned char *p;
	int len;

	p = strbuf.val;

	while (p < (strbuf.val + strbuf.len)) {
		len = strlen(p);
		fprintf(f, "\t.string \"%s\"\n", p);
		p += len+1;
	}
}

void dt_to_asm(FILE *f, struct boot_info *bi, int version, int boot_cpuid_phys)
{
	struct version_info *vi = NULL;
	int i;
	struct data strbuf = empty_data;
	struct reserve_info *re;
	char *symprefix = "dt";

	for (i = 0; i < ARRAY_SIZE(version_table); i++) {
		if (version_table[i].version == version)
			vi = &version_table[i];
	}
	if (!vi)
		die("Unknown device tree blob version %d\n", version);

	fprintf(f, "/* autogenerated by dtc, do not edit */\n\n");
	fprintf(f, "#define OF_DT_HEADER 0x%x\n", OF_DT_HEADER);
	fprintf(f, "#define OF_DT_BEGIN_NODE 0x%x\n", OF_DT_BEGIN_NODE);
	fprintf(f, "#define OF_DT_END_NODE 0x%x\n", OF_DT_END_NODE);
	fprintf(f, "#define OF_DT_PROP 0x%x\n", OF_DT_PROP);
	fprintf(f, "#define OF_DT_END 0x%x\n", OF_DT_END);
	fprintf(f, "\n");

	emit_label(f, symprefix, "blob_start");
	emit_label(f, symprefix, "header");
	fprintf(f, "\t.long\tOF_DT_HEADER /* magic */\n");
	fprintf(f, "\t.long\t_%s_blob_end - _%s_blob_start /* totalsize */\n",
		symprefix, symprefix);
	fprintf(f, "\t.long\t_%s_struct_start - _%s_blob_start /* off_dt_struct */\n",
		symprefix, symprefix);
	fprintf(f, "\t.long\t_%s_strings_start - _%s_blob_start /* off_dt_strings */\n",
		symprefix, symprefix);
	fprintf(f, "\t.long\t_%s_reserve_map - _%s_blob_start /* off_dt_strings */\n",
		symprefix, symprefix);
	fprintf(f, "\t.long\t%d /* version */\n", vi->version);
	fprintf(f, "\t.long\t%d /* last_comp_version */\n",
		vi->last_comp_version);

	if (vi->flags & FTF_BOOTCPUID)
		fprintf(f, "\t.long\t%i\t/*boot_cpuid_phys*/\n",
			boot_cpuid_phys);

	if (vi->flags & FTF_STRTABSIZE)
		fprintf(f, "\t.long\t_%s_strings_end - _%s_strings_start\t/* size_dt_strings */\n",
			symprefix, symprefix);

	/*
	 * Reserve map entries.
	 * Align the reserve map to a doubleword boundary.
	 * Each entry is an (address, size) pair of u64 values.
	 * Always supply a zero-sized temination entry.
	 */
	asm_emit_align(f, 8);
	emit_label(f, symprefix, "reserve_map");

	fprintf(f, "/* Memory reserve map from source file */\n");

	/*
	 * Use .long on high and low halfs of u64s to avoid .quad
	 * as it appears .quad isn't available in some assemblers.
	 */
	for (re = bi->reservelist; re; re = re->next) {
		fprintf(f, "\t.long\t0x%08x\n\t.long\t0x%08x\n",
			(unsigned int)(re->re.address >> 32),
			(unsigned int)(re->re.address & 0xffffffff));
		fprintf(f, "\t.long\t0x%08x\n\t.long\t0x%08x\n",
			(unsigned int)(re->re.size >> 32),
			(unsigned int)(re->re.size & 0xffffffff));
	}

	fprintf(f, "\t.long\t0, 0\n\t.long\t0, 0\n");

	emit_label(f, symprefix, "struct_start");
	flatten_tree(bi->dt, &asm_emitter, f, &strbuf, vi);
	fprintf(f, "\t.long\tOF_DT_END\n");
	emit_label(f, symprefix, "struct_end");

	emit_label(f, symprefix, "strings_start");
	dump_stringtable_asm(f, strbuf);
	emit_label(f, symprefix, "strings_end");

	emit_label(f, symprefix, "blob_end");

	data_free(strbuf);
}

static void dump_stringtable_C(FILE *f, struct data strbuf)
{
	unsigned char *p;
	int len;

	p = strbuf.val;

	fprintf(f, "\tchar *stringtable[] = {\n");
	while (p < (strbuf.val + strbuf.len)) {
		len = strlen(p);
		fprintf(f, "\t\"%s\"\n", p);
		p += len+1;
	}
	fprintf(f, "\t};\n");

}


void dt_to_C(FILE *f, struct boot_info *bi, int version, int boot_cpuid_phys)
{
	struct version_info *vi = NULL;
	int i;
	struct data strbuf = empty_data;
	struct reserve_info *re;
	char *symprefix = "dt";

	for (i = 0; i < ARRAY_SIZE(version_table); i++) {
		if (version_table[i].version == version)
			vi = &version_table[i];
	}
	if (!vi)
		die("Unknown device tree blob version %d\n", version);

	fprintf(f, "/* autogenerated by dtc, do not edit */\n\n");
	fprintf(f, "#define OF_DT_HEADER 0x%x\n", OF_DT_HEADER);
	fprintf(f, "#define OF_DT_BEGIN_NODE 0x%x\n", OF_DT_BEGIN_NODE);
	fprintf(f, "#define OF_DT_END_NODE 0x%x\n", OF_DT_END_NODE);
	fprintf(f, "#define OF_DT_PROP 0x%x\n", OF_DT_PROP);
	fprintf(f, "#define OF_DT_END 0x%x\n", OF_DT_END);
	fprintf(f, "\n");

	C_emit_struct_start(f, symprefix, "blob");
	fprintf(f, "\tu32\tmagic = OF_DT_HEADER;\n");
	fprintf(f, "\tu32\ttotalsize = sizeof(struct blob);\n");
	fprintf(f, "\tu32\toffdt = offsetof(struct blob, dt);\n");
	fprintf(f, "\tu32\toffstrings = offsetof(struct blob, strings);\n");
	fprintf(f, "\tu32\toffreserve = offsetof(struct blob, reserve);\n");
	fprintf(f, "\tu32\tversion = %d;\n", vi->version);
	fprintf(f, "\tu32\tlast_comp_version = %d;\n",
		vi->last_comp_version);

	if (vi->flags & FTF_BOOTCPUID)
		fprintf(f, "\tu32\tboot_cpuid_phys = 0x%x;\n",
			boot_cpuid_phys);

	if (vi->flags & FTF_STRTABSIZE)
		fprintf(f, "\tu32\tsize_%s_strings_end = sizeof(dt.strings);\n",
			symprefix);
	/*
	 * Reserve map entries.
	 * Align the reserve map to a doubleword boundary.
	 * Each entry is an (address, size) pair of u64 values.
	 * Always supply a zero-sized temination entry.
	 */
	C_emit_align(f, 8);

	fprintf(f, "\t/* Memory reserve map from source file */\n");

	/*
	 * Use .long on high and low halfs of u64s to avoid .quad
	 * as it appears .quad isn't available in some assemblers.
	 */
	fprintf(f, "\tu64 reservemap[] = {\n");
	for (re = bi->reservelist; re; re = re->next) {
		fprintf(f, "\tu64\t0x%lx\n", re->re.address);
		fprintf(f, "\tu64\t0x%lx\n", re->re.size);
	}


	fprintf(f, "\t0, 0\n");
	fprintf(f, "\t};\n");

	C_emit_struct_start(f, symprefix, "dt_blob");
	flatten_tree(bi->dt, &C_emitter, f, &strbuf, vi);
	fprintf(f, "\tu32\tend = OF_DT_END;\n");
	C_emit_struct_end(f, symprefix, "dt_blob");

	dump_stringtable_C(f, strbuf);

	C_emit_struct_end(f, symprefix, "blob");

	data_free(strbuf);
}

/* the label is not really used. So go ahead and make clean names for all labels */

void
labeltree(struct node *tree)
{
	struct node *child;

	tree->label = clean(tree->name, 1);
	
	if (tree->next_sibling)
		labeltree(tree->next_sibling);

	for_each_child(tree, child) {
		labeltree(child);
	}

}

/* the root, weirdly enough, is last on the 'next' chain. yuck. */
void fix_next(struct node *root){
	extern struct node *first_node;
	struct node *next2last=NULL, *next;
	for(next = first_node; next; next = next->next)
		if (next->next == root)
			next2last = next;
	next2last->next = NULL;
	root->next = first_node;
	first_node = root;
}

void dt_to_linuxbios(FILE *f, struct boot_info *bi, int version, int boot_cpuid_phys)
{
	struct version_info *vi = NULL;
	int i;
	struct data strbuf = empty_data;
	char *symprefix = "dt";
	extern char *code;
	struct node *next;
	extern struct node *first_node;

	labeltree(bi->dt);

	for (i = 0; i < ARRAY_SIZE(version_table); i++) {
		if (version_table[i].version == version)
			vi = &version_table[i];
	}
	if (!vi)
		die("Unknown device tree blob version %d\n", version);

	/* the root is special -- the parser gives it no name. We fix that here. 
	  * fix in parser? 
	  */
	bi->dt->name  = bi->dt->label  = "root";
	/* steps: emit all structs. Then emit the initializers, with the pointers to other structs etc. */

	fix_next(bi->dt);
	/* emit any includes that we need  -- TODO: ONLY ONCE PER TYPE*/
	fprintf(f, "#include <device/device.h>\n#include <device/pci.h>\n");
	flatten_tree_emit_includes(bi->dt, &linuxbios_emitter, f, &strbuf, vi);
	/* forward declarations */
	for(next = first_node; next; next = next->next)
		fprintf(f, "struct device dev_%s;\n", next->label);

	/* emit the code, if any */
	if (code)
		fprintf(f, "%s\n", code);


//	flatten_tree_emit_structdecls(bi->dt, &linuxbios_emitter, f, &strbuf, vi);
	flatten_tree_emit_structinits(bi->dt, &linuxbios_emitter, f, &strbuf, vi);
	data_free(strbuf);
	/* */

}


struct inbuf {
	char *base, *limit, *ptr;
};

static void inbuf_init(struct inbuf *inb, void *base, void *limit)
{
	inb->base = base;
	inb->limit = limit;
	inb->ptr = inb->base;
}

static void flat_read_chunk(struct inbuf *inb, void *p, int len)
{
	if ((inb->ptr + len) > inb->limit)
		die("Premature end of data parsing flat device tree\n");

	memcpy(p, inb->ptr, len);

	inb->ptr += len;
}

static u32 flat_read_word(struct inbuf *inb)
{
	u32 val;

	assert(((inb->ptr - inb->base) % sizeof(val)) == 0);

	flat_read_chunk(inb, &val, sizeof(val));

	return be32_to_cpu(val);
}

static void flat_realign(struct inbuf *inb, int align)
{
	int off = inb->ptr - inb->base;

	inb->ptr = inb->base + ALIGN(off, align);
	if (inb->ptr > inb->limit)
		die("Premature end of data parsing flat device tree\n");
}

static char *flat_read_string(struct inbuf *inb)
{
	int len = 0;
	char *p = inb->ptr;
	char *str;

	do {
		if (p >= inb->limit)
			die("Premature end of data parsing flat device tree\n");
		len++;
	} while ((*p++) != '\0');

	str = strdup(inb->ptr);

	inb->ptr += len;

	flat_realign(inb, sizeof(u32));

	return str;
}

static struct data flat_read_data(struct inbuf *inb, int len)
{
	struct data d = empty_data;

	if (len == 0)
		return empty_data;

	d = data_grow_for(d, len);
	d.len = len;

	flat_read_chunk(inb, d.val, len);

	flat_realign(inb, sizeof(u32));

	return d;
}

static char *flat_read_stringtable(struct inbuf *inb, int offset)
{
	char *p;

	p = inb->base + offset;
	while (1) {
		if (p >= inb->limit || p < inb->base)
			die("String offset %d overruns string table\n",
			    offset);

		if (*p == '\0')
			break;

		p++;
	}

	return strdup(inb->base + offset);
}

static struct property *flat_read_property(struct inbuf *dtbuf,
					   struct inbuf *strbuf, int flags)
{
	u32 proplen, stroff;
	char *name;
	struct data val;

	proplen = flat_read_word(dtbuf);
	stroff = flat_read_word(dtbuf);

	name = flat_read_stringtable(strbuf, stroff);

	if ((flags & FTF_VARALIGN) && (proplen >= 8))
		flat_realign(dtbuf, 8);

	val = flat_read_data(dtbuf, proplen);

	return build_property(name, val, NULL);
}


static struct reserve_info *flat_read_mem_reserve(struct inbuf *inb)
{
	struct reserve_info *reservelist = NULL;
	struct reserve_info *new;
	char *p;
	struct reserve_entry re;

	/*
	 * Each entry is a pair of u64 (addr, size) values for 4 cell_t's.
	 * List terminates at an entry with size equal to zero.
	 *
	 * First pass, count entries.
	 */
	p = inb->ptr;
	while (1) {
		flat_read_chunk(inb, &re, sizeof(re));
		re.address  = be64_to_cpu(re.address);
		re.size = be64_to_cpu(re.size);
		if (re.size == 0)
			break;

		new = build_reserve_entry(re.address, re.size, NULL);
		reservelist = add_reserve_entry(reservelist, new);
	}

	return reservelist;
}


static char *nodename_from_path(char *ppath, char *cpath)
{
	char *lslash;
	int plen;

	lslash = strrchr(cpath, '/');
	if (! lslash)
		return NULL;

	plen = lslash - cpath;

	if (streq(cpath, "/") && streq(ppath, ""))
		return "";

	if ((plen == 0) && streq(ppath, "/"))
		return strdup(lslash+1);

	if (! strneq(ppath, cpath, plen))
		return NULL;
	
	return strdup(lslash+1);
}

static const char PROPCHAR[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,._+*#?-";
static const char UNITCHAR[] = "0123456789abcdef,";

static int check_node_name(char *name)
{
	char *atpos;
	unsigned int basenamelen;

	atpos = strrchr(name, '@');

	if (atpos)
		basenamelen = atpos - name;
	else
		basenamelen = strlen(name);

	if (strspn(name, PROPCHAR) < basenamelen)
		return -1;

	if (atpos
	    && ((basenamelen + 1 + strspn(atpos+1, UNITCHAR)) < strlen(name)))
		return -1;

	return basenamelen;
}

static struct node *unflatten_tree(struct inbuf *dtbuf,
				   struct inbuf *strbuf,
				   char *parent_path, int flags)
{
	struct node *node;
	u32 val;

	node = build_node(NULL, NULL);

	if (flags & FTF_FULLPATH) {
		node->fullpath = flat_read_string(dtbuf);
		node->name = nodename_from_path(parent_path, node->fullpath);

		if (! node->name)
			die("Path \"%s\" is not valid as a child of \"%s\"\n",
			    node->fullpath, parent_path);
	} else {
		node->name = flat_read_string(dtbuf);
		node->fullpath = join_path(parent_path, node->name);
	}
	
	node->basenamelen = check_node_name(node->name);
	if (node->basenamelen < 0) {
		fprintf(stderr, "Warning \"%s\" has incorrect format\n", node->name);
	}

	do {
		struct property *prop;
		struct node *child;

		val = flat_read_word(dtbuf);
		switch (val) {
		case OF_DT_PROP:
			prop = flat_read_property(dtbuf, strbuf, flags);
			add_property(node, prop);
			break;

		case OF_DT_BEGIN_NODE:
			child = unflatten_tree(dtbuf,strbuf, node->fullpath,
					       flags);
			add_child(node, child);
			break;

		case OF_DT_END_NODE:
			break;

		case OF_DT_END:
			die("Premature OF_DT_END in device tree blob\n");
			break;

		default:
			die("Invalid opcode word %08x in device tree blob\n",
			    val);
		}
	} while (val != OF_DT_END_NODE);

	return node;
}


struct boot_info *dt_from_blob(FILE *f)
{
	u32 magic, totalsize, version, size_str = 0;
	u32 off_dt, off_str, off_mem_rsvmap;
	int rc;
	char *blob;
	struct boot_param_header *bph;
	char *p;
	struct inbuf dtbuf, strbuf;
	struct inbuf memresvbuf;
	int sizeleft;
	struct reserve_info *reservelist;
	struct node *tree;
	u32 val;
	int flags = 0;

	rc = fread(&magic, sizeof(magic), 1, f);
	if (ferror(f))
		die("Error reading DT blob magic number: %s\n",
		    strerror(errno));
	if (rc < 1) {
		if (feof(f))
			die("EOF reading DT blob magic number\n");
		else
			die("Mysterious short read reading magic number\n");
	}

	magic = be32_to_cpu(magic);
	if (magic != OF_DT_HEADER)
		die("Blob has incorrect magic number\n");

	rc = fread(&totalsize, sizeof(totalsize), 1, f);
	if (ferror(f))
		die("Error reading DT blob size: %s\n", strerror(errno));
	if (rc < 1) {
		if (feof(f))
			die("EOF reading DT blob size\n");
		else
			die("Mysterious short read reading blob size\n");
	}

	totalsize = be32_to_cpu(totalsize);
	if (totalsize < BPH_V1_SIZE)
		die("DT blob size (%d) is too small\n", totalsize);

	blob = xmalloc(totalsize);

	bph = (struct boot_param_header *)blob;
	bph->magic = cpu_to_be32(magic);
	bph->totalsize = cpu_to_be32(totalsize);

	sizeleft = totalsize - sizeof(magic) - sizeof(totalsize);
	p = blob + sizeof(magic)  + sizeof(totalsize);

	while (sizeleft) {
		if (feof(f))
			die("EOF before reading %d bytes of DT blob\n",
			    totalsize);

		rc = fread(p, 1, sizeleft, f);
		if (ferror(f))
			die("Error reading DT blob: %s\n",
			    strerror(errno));

		sizeleft -= rc;
		p += rc;
	}

	off_dt = be32_to_cpu(bph->off_dt_struct);
	off_str = be32_to_cpu(bph->off_dt_strings);
	off_mem_rsvmap = be32_to_cpu(bph->off_mem_rsvmap);
	version = be32_to_cpu(bph->version);

	fprintf(stderr, "\tmagic:\t\t\t0x%x\n", magic);
	fprintf(stderr, "\ttotalsize:\t\t%d\n", totalsize);
	fprintf(stderr, "\toff_dt_struct:\t\t0x%x\n", off_dt);
	fprintf(stderr, "\toff_dt_strings:\t\t0x%x\n", off_str);
	fprintf(stderr, "\toff_mem_rsvmap:\t\t0x%x\n", off_mem_rsvmap);
	fprintf(stderr, "\tversion:\t\t0x%x\n", version );
	fprintf(stderr, "\tlast_comp_version:\t0x%x\n",
		be32_to_cpu(bph->last_comp_version));

	if (off_mem_rsvmap >= totalsize)
		die("Mem Reserve structure offset exceeds total size\n");

	if (off_dt >= totalsize)
		die("DT structure offset exceeds total size\n");

	if (off_str > totalsize)
		die("String table offset exceeds total size\n");

	if (version >= 2)
		fprintf(stderr, "\tboot_cpuid_phys:\t0x%x\n",
			be32_to_cpu(bph->boot_cpuid_phys));

	if (version >= 3) {
		size_str = be32_to_cpu(bph->size_dt_strings);
		fprintf(stderr, "\tsize_dt_strings:\t%d\n", size_str);
		if (off_str+size_str > totalsize)
			die("String table extends past total size\n");
	}
			
	if (version < 0x10) {
		flags |= FTF_FULLPATH | FTF_NAMEPROPS | FTF_VARALIGN;
	}

	inbuf_init(&memresvbuf,
		   blob + off_mem_rsvmap, blob + totalsize);
	inbuf_init(&dtbuf, blob + off_dt, blob + totalsize);
	inbuf_init(&strbuf, blob + off_str, blob + totalsize);

	if (version >= 3)
		strbuf.limit = strbuf.base + size_str;

	reservelist = flat_read_mem_reserve(&memresvbuf);

	val = flat_read_word(&dtbuf);

	if (val != OF_DT_BEGIN_NODE)
		die("Device tree blob doesn't begin with OF_DT_BEGIN_NODE (begins with 0x%08x)\n", val);

	tree = unflatten_tree(&dtbuf, &strbuf, "", flags);

	val = flat_read_word(&dtbuf);
	if (val != OF_DT_END)
		die("Device tree blob doesn't end with OF_DT_END\n");

	free(blob);

	return build_boot_info(reservelist, tree);
}
