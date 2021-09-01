/*
 * Copyright (c) 2021 Min Le (lemin9538@163.com)
 */

#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <dirent.h>

#include <minos/list.h>
#include <minos/types.h>
#include <minos/debug.h>
#include <minos/kmalloc.h>
#include <minos/compiler.h>

#include <libminos/vfs.h>
#include <libminos/blkdev.h>

#define FAT12				0
#define FAT16				1
#define FAT32				2

#define FAT_NAME_TYPE_SHORT		0
#define FAT_NAME_TYPE_LONG		1	
#define FAT_NAME_TYPE_UNKNOWN		0xff

#define FAT_LONG_NAME_MIN_MATCH_SIZE	6

struct fs_info {
	uint32_t lead_sig;		/* 0 */
	uint32_t struct_sig;		/* 484 */
	uint32_t free_count;		/* 488 */
	uint32_t next_free;		/* 492 */
};

struct fat32_extra {
	uint32_t fat32_sec_cnt;
	uint16_t fat32_ext_flag;
	uint16_t fs_ver;
	uint32_t root_clus;
	uint16_t fs_info;
	uint16_t boot_sec;
	char res[12];
	uint8_t drv_num;
	uint8_t res1;
	uint8_t boot_sig;
	uint32_t vol_id;
	char vol_lab[11];
	char file_system[8];		/* fat12 or fat16 or fat32 */
};

struct fat16_extra {
	uint8_t drv_num;
	uint8_t res;
	uint8_t boot_sig;
	uint32_t vol_id;
	char vol_lab[11];
	char file_system[8];		/* fat12 or fat16 or fat32 */
};

struct fat_super_block {
	uint8_t jmp[3];
	char oem_name[8];
	uint16_t byts_per_sec;
	uint8_t sec_per_clus;
	uint16_t res_sec_cnt;
	uint8_t fat_num;
	uint16_t root_ent_cnt;
	uint16_t total_sec16;
	uint8_t media;
	uint16_t fat16_sec_size;
	uint16_t sec_per_trk;
	uint16_t num_heads;
	uint32_t hide_sec;
	uint32_t total_sec32;
	union _fat_extra {
		struct fat32_extra fat32;
		struct fat16_extra fat16;
	} fat_extra;

	/* to be done */
	struct fs_info info;

	uint32_t clus_size;		/* bytes_per_sec * sec_per_clus */
	uint32_t first_data_sector;
	uint32_t total_sec;
	uint16_t fat_size;
	uint32_t data_sec;
	uint32_t clus_count;
	uint8_t fat_type;
	uint8_t fat_offset;
	uint32_t root_dir_sectors;
	uint16_t dentry_per_block;
	uint16_t root_dir_start_sector;
	uint16_t fat12_16_root_dir_blk;

	/* for fat buffer only for read, if need write
	 * need request a buffer pool from system buffer
	 */
	uint32_t buf_sector_base;
	char *buf;

	struct super_block sb;
};

struct fat_fnode {
	uint32_t first_clus;
	uint32_t first_sector;
	uint32_t prev_clus;
	uint32_t prev_sector;
	uint32_t current_clus;
	uint32_t current_sector;
	uint32_t file_entry_clus;		/* information of the file entry */
	uint16_t file_entry_pos;
	struct fnode fnode;
};

struct fat_dir_entry {
	char name[8];		/* name[0] = 0xe5 the dir is empty */
	char externed[3];
	uint8_t dir_attr;
	uint8_t nt_res;
	uint8_t crt_time_teenth;
	uint16_t crt_time;
	uint16_t crt_date;
	uint16_t last_acc_date;
	uint16_t fst_cls_high;
	uint16_t write_time;
	uint16_t write_date;
	uint16_t fst_cls_low;
	uint32_t file_size;
} __attribute__((packed));

struct fat_long_dir_entry {
	uint8_t attr;
	uint16_t name1[5];
	uint8_t dir_attr;
	uint8_t res;
	uint8_t check_sum;
	uint16_t name2[6];
	uint16_t file_start_clus;
	uint16_t name3[2];
}__attribute__((packed));

struct fat_name {
	char short_name[11];
	char is_long;
	uint16_t *long_name;
};

#define FIRST_DATA_BLOCK	2

#define FAT_ATTR_READ_ONLY	0x01
#define FAT_ATTR_HIDDEN		0x02
#define FAT_ATTR_SYSTEM		0X04
#define FAT_ATTR_VOLUME_ID	0x08
#define FAT_ATTR_DIRECTORY	0x10
#define FAT_ATTR_ARCHIVE	0x20

#define FAT_ATTR_LONG_NAME	\
	(FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | \
	FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

#define FAT_ATTR_LONG_NAME_MASK	\
	(FAT_ATTR_LONG_NAME | FAT_ATTR_DIRECTORY | FAT_ATTR_ARCHIVE)

#define SB_TO_FAT_SB(_sb)	container_of(_sb, struct fat_super_block, sb)
#define FNODE_TO_FAT_FNODE(node) container_of(node, struct fat_fnode, fnode)

static inline uint32_t get_first_sector(struct fat_super_block *fsb, uint32_t clus)
{
	if (clus > (0xfff7 - fsb->fat12_16_root_dir_blk))	/* root dir for fat12 or fat 16*/
		return fsb->root_dir_start_sector + ((clus - 0xfff7) * fsb->sec_per_clus);
	else
		return ((clus - 2) * fsb->sec_per_clus) + fsb->first_data_sector;
}

static int fat_read_block(struct super_block *sb, char *buffer, uint32_t block)
{
	struct fat_super_block *fsb = SB_TO_FAT_SB(sb);
	uint32_t start_sector = get_first_sector(fsb, block);

	return read_blkdev_sectors(sb->partition->blkdev, buffer,
			start_sector, fsb->sec_per_clus);
}

static int fat_write_block(struct super_block *sb, char *buffer, uint32_t block)
{
	struct fat_super_block *fsb = SB_TO_FAT_SB(sb);
	uint32_t start_sector = get_first_sector(fsb, block);

	return write_blkdev_sectors(sb->partition->blkdev, buffer,
			start_sector, fsb->sec_per_clus);
}

static uint8_t get_fat_type(struct fat_super_block *fsb, char *tmp)
{
	uint32_t fat_size, total_sec;
	uint32_t data_sec, root_dir_sectors, clus_count;

	/* following is the way to check the fat fs type */
	root_dir_sectors = ((fsb->root_ent_cnt * 32) +
			   (fsb->byts_per_sec - 1)) /
		           fsb->byts_per_sec;

	if (fsb->fat16_sec_size != 0)
		fat_size = fsb->fat16_sec_size;
	else
		fat_size = u8_to_u32(tmp[36], tmp[37], tmp[38], tmp[39]);

	if (fsb->total_sec16 != 0)
		total_sec = fsb->total_sec16;
	else
		total_sec = fsb->total_sec32;

	data_sec = total_sec -
		(fsb->res_sec_cnt + fsb->fat_num * fat_size + root_dir_sectors);
	clus_count = data_sec / fsb->sec_per_clus;

	if (clus_count < 4085)
		fsb->fat_type = FAT12;
	else if (clus_count < 65525)
		fsb->fat_type = FAT16;
	else
		fsb->fat_type = FAT32;

	fsb->total_sec = total_sec;
	fsb->fat_size = fat_size;
	fsb->data_sec = data_sec;
	fsb->clus_count = clus_count;
	fsb->root_dir_sectors = root_dir_sectors;
	fsb->root_dir_start_sector = fsb->res_sec_cnt + fsb->fat_num * fat_size;

	/* do not support fat12 */
	if (fsb->fat_type == FAT32)
		fsb->fat_offset = 4;
	else
		fsb->fat_offset = 2;

	return fsb->fat_type;
}

static uint32_t clus_in_fat(struct fat_super_block *fsb, uint32_t clus, int *i, int *j)
{
	uint32_t offset;

	if (fsb->fat_type == FAT12)
		offset = clus + (clus / 2);
	else
		offset = fsb->fat_offset * clus;

	*i = fsb->res_sec_cnt + (offset / fsb->byts_per_sec);
	*j = offset % fsb->byts_per_sec;

	return 0;
}

static int fill_fat_super(struct fat_super_block *fsb, char *buf)
{
	uint8_t *tmp = (uint8_t *)buf;
	struct fat32_extra *extra32;
	struct fat16_extra *extra16;
	uint8_t fat_type;

	if ((tmp[510] != 0x55) || (tmp[511] != 0xaa))
		return -EINVAL;

	memcpy(fsb->jmp, tmp, 3);
	memcpy(fsb->oem_name, &tmp[3], 8);
	fsb->byts_per_sec = u8_to_u16(tmp[11], tmp[12]); // must equal disk->sector_size
	fsb->sec_per_clus = tmp[13];
	fsb->res_sec_cnt = u8_to_u16(tmp[14], tmp[15]);
	fsb->fat_num = tmp[16];
	fsb->root_ent_cnt = u8_to_u16(tmp[17], tmp[18]);
	fsb->total_sec16 = u8_to_u16(tmp[19], tmp[20]);
	fsb->media = tmp[21];
	fsb->fat16_sec_size = u8_to_u16(tmp[22], tmp[23]);
	fsb->sec_per_trk = u8_to_u16(tmp[24], tmp[25]);
	fsb->num_heads = u8_to_u16(tmp[26], tmp[27]);
	fsb->hide_sec = u8_to_u32(tmp[28], tmp[29], tmp[30], tmp[31]);
	fsb->total_sec32 = u8_to_u32(tmp[32], tmp[33], tmp[34], tmp[35]);

	fat_type = get_fat_type(fsb, buf);
	if ((fat_type == FAT16 || (fat_type == FAT12))) {
		extra16 = &fsb->fat_extra.fat16;
		extra16->drv_num = tmp[36];
		extra16->boot_sig = tmp[38];
		extra16->vol_id = u8_to_u32(tmp[39], tmp[40], tmp[41], tmp[42]);
		memcpy(extra16->vol_lab, &tmp[43], 11);
		memcpy(extra16->file_system, &tmp[54], 8);
	} else {
		extra32 = &fsb->fat_extra.fat32;
		extra32->fat32_sec_cnt = u8_to_u32(tmp[36], tmp[37], tmp[38], tmp[39]);
		extra32->fs_ver = u8_to_u16(tmp[42], tmp[43]);
		extra32->root_clus = u8_to_u32(tmp[44], tmp[45], tmp[46], tmp[47]);
		extra32->fs_info = u8_to_u16(tmp[48], tmp[49]);
		extra32->boot_sec = u8_to_u16(tmp[50], tmp[51]);
		extra32->drv_num = tmp[64];
		extra32->boot_sig = tmp[66];
		extra32->vol_id = u8_to_u32(tmp[67], tmp[68], tmp[69], tmp[70]);
		memcpy(extra32->vol_lab, &tmp[71], 11);
		memcpy(extra32->file_system, &tmp[82], 8);
	}

	fsb->clus_size = fsb->byts_per_sec * fsb->sec_per_clus;
	fsb->first_data_sector = fsb->res_sec_cnt +
		(fsb->fat_num * fsb->fat_size) + fsb->root_dir_sectors;
	fsb->dentry_per_block = fsb->clus_size / 32;
	fsb->fat12_16_root_dir_blk = (fsb->root_dir_sectors +
			(fsb->sec_per_clus - 1)) / fsb->sec_per_clus;

	return 0;
}

static int fat_get_root_file(struct super_block *sb)
{
	struct fat_super_block *fsb = SB_TO_FAT_SB(sb);
	struct fnode *fnode;
	struct fat_fnode *ffnode;

	ffnode = kzalloc(sizeof(struct fat_fnode) + sizeof(struct fnode));
	if (!ffnode)
		return -ENOMEM;
	fnode = &ffnode->fnode;

	if (fsb->fat_type == FAT32)
		ffnode->first_clus = 2;
	else
		ffnode->first_clus = 0xfff7;
	ffnode->prev_clus = ffnode->first_clus;
	ffnode->current_clus = ffnode->first_clus;

	init_list(&fnode->child);
	fnode->type = DT_DIR;
	sb->root_fnode = fnode;

	return 0;
}

static int fat_create_super_block(struct partition *partition,
		struct filesystem *fs)
{
	struct fat_super_block *fat_super;
	struct super_block *sb;
	char *buf;
	int ret = 0;

	buf = get_pages(partition->blkdev->pages_per_sector);
	if (!buf)
		return -ENOMEM;

	ret = read_blkdev_sectors(partition->blkdev, buf, partition->lba, 1);
	if (ret)
		goto err_read_block;

	fat_super = kzalloc(sizeof(struct fat_super_block));
	if (!fat_super) {
		ret = -ENOMEM;
		goto err_read_block;
	}

	sb = &fat_super->sb;
	ret = fill_fat_super(fat_super, buf);
	if (ret)
		goto err_get_super_block;

	if (fat_super->byts_per_sec != partition->blkdev->sector_size) {
		pr_err("fat sector size is not equal disk's %d\n",
				fat_super->byts_per_sec);
		ret = -EINVAL;
		goto err_get_super_block;
	}

	/*
	 * allocate one clus memory to buffer some important
	 * data.
	 */
	fat_super->buf = get_pages(PAGE_NR(fat_super->clus_size));
	if (!fat_super->buf) {
		ret = -ENOMEM;
		goto err_get_super_block;
	}
	fat_super->buf_sector_base = 0;

	pr_debug("------Fat information-----\n");
	pr_debug("clus size :%d\n", fat_super->clus_size);
	pr_debug("first_data_sector: %d\n", fat_super->first_data_sector);
	pr_debug("total_sec: %d\n", fat_super->total_sec);
	pr_debug("fat_size: %d\n", fat_super->fat_size);
	pr_debug("data_sec: %d\n", fat_super->data_sec);
	pr_debug("clus_count: %d\n", fat_super->clus_count);
	pr_debug("root_dir_sectors: %d\n", fat_super->root_dir_sectors);
	pr_debug("dentry_per_block: %d\n", fat_super->dentry_per_block);
	pr_debug("fat_type :%d\n", fat_super->fat_type);
	pr_debug("fat_offset: %d\n", fat_super->fat_offset);
	pr_debug("byts_per_sec: %d\n", fat_super->byts_per_sec);
	pr_debug("sec_per_clus: %d\n", fat_super->sec_per_clus);
	pr_debug("res_sec_cnt: %d\n", fat_super->res_sec_cnt);
	pr_debug("root_ent_cnt: %d\n", fat_super->root_ent_cnt);
	pr_debug("root_dir_start_secrot: %d\n", fat_super->root_dir_start_sector);
	pr_debug("fat12_16_root_dir_blk: %d\n", fat_super->fat12_16_root_dir_blk);
	pr_debug("------------------------\n");

	ret = fat_get_root_file(sb);
	if (ret)
		goto err_get_root_file;

	sb->block_size_bits = 12;
	sb->block_size = 4096;
	sb->max_file = (uint32_t)-1;	// TBD
	sb->flags = 0;
	sb->magic = 0;
	sb->partition = partition;
	partition->sb = sb;
	free_pages(buf);

	return 0;

err_get_root_file:
	free_pages(fat_super->buf);
err_get_super_block:
	kfree(sb);
err_read_block:
	free_pages(buf);

	return ret;
}

static int fat_file_type(struct fat_dir_entry *entry)
{
	int type = DT_UNKNOWN;
	int attr = entry->dir_attr;
	
	/* if it is not a long name file */
	if ((attr & FAT_ATTR_LONG_NAME_MASK) != FAT_ATTR_LONG_NAME) {
		if (attr & FAT_ATTR_DIRECTORY)
			type = DT_DIR;
		else if (attr & FAT_ATTR_VOLUME_ID)
			type = FAT_ATTR_VOLUME_ID;
		else if ((attr & (FAT_ATTR_VOLUME_ID | FAT_ATTR_DIRECTORY)) == 0)
			type = DT_BLK;
		else
			pr_info("unknow fat file type");
	}

	return type;
}

static inline int fat_entry_is_long_name(struct fat_dir_entry *entry)
{
	return ((entry->dir_attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME);
}

static int cmp_long_name(char *buf, int i, char *long_name, int size)
{
	struct fat_long_dir_entry *lentry =
		((struct fat_long_dir_entry *)buf) + i;
	uint8_t cmp_size[3] = {10, 12, 4};
	int _cmp_size = 0;
	int j;
	
	do {
		for (j = 0; j < 3; j++) {
			_cmp_size = MIN(size, cmp_size[j]);
			if (strncmp(long_name, (char *)lentry->name1, _cmp_size))
				goto out;

			long_name += _cmp_size;
			size -= _cmp_size;
			if (size == 0)
				return 0;
		}
		
		lentry--;
		i--;
		if (i < 0) {
			/* 
			 * the info is in different clus load
			 * the previous clus data
			 */
#if 0
			if (fat_read_block(root->sb, buf, ffnode->prev_clus))
				return -EIO;

			lentry = ((struct fat_long_dir_entry *)buf) +
				 (fsb->dentry_per_block - 1);
			i = fsb->dentry_per_block - 1;
#endif
			pr_warn("long name do not in same cluster\n");
		}
	} while (size > 0);

out:
	return 1;
}

static int fat_fill_fnode(struct fat_fnode *ffnode, struct fat_super_block *fsb,
		struct fat_dir_entry *entry)
{
	struct fnode *fnode = &ffnode->fnode;

	if (fsb->fat_type == FAT32)
		ffnode->first_clus = u16_to_u32(entry->fst_cls_low,
					entry->fst_cls_high);
	else
		ffnode->first_clus = entry->fst_cls_low;
	
	ffnode->first_sector = get_first_sector(fsb, ffnode->first_clus);
	ffnode->prev_clus = ffnode->first_clus;
	ffnode->prev_sector = ffnode->first_sector;
	ffnode->current_clus = ffnode->first_clus;
	ffnode->current_sector = ffnode->first_sector;
	ffnode->file_entry_clus = ffnode->current_clus;

	fnode->type = fat_file_type(entry);
	fnode->mode = entry->dir_attr;
	fnode->flags = entry->dir_attr;
	fnode->atime = u16_to_u32(entry->crt_date, entry->crt_time);
	fnode->mtime = u16_to_u32(entry->write_date, entry->write_time);
	fnode->ctime = entry->crt_time_teenth;
	fnode->file_size = entry->file_size;
	fnode->state = 0;

	return 0;
}

static int sector_in_fat_buffer(struct fat_super_block *fsb, int i, int j)
{
	if (fsb->buf_sector_base == 0)
		return 0;

	if ((i >= fsb->buf_sector_base) && (i < fsb->buf_sector_base + fsb->sec_per_clus))
		return ((i - fsb->buf_sector_base) * fsb->byts_per_sec) / fsb->fat_offset + j;

	return 0;
}

static uint32_t fat_get_next_data_block(struct fat_super_block *fsb, uint32_t current_clus)
{
	char *buf = fsb->buf;
	int i, j, k;
	uint32_t next;

	clus_in_fat(fsb, current_clus, &i, &j);
	k = sector_in_fat_buffer(fsb, i, j);
	pr_debug("%s i:%d j:%d k:%d\n", __func__, i, j, k);

	if (!k) {
		if (fat_read_block(&fsb->sb, buf, i))
			return 0;

		fsb->buf_sector_base = i;
		k = j;
	} else {
		/*
		 * TBD
		 */
		if (fsb->fat_type == FAT12) {
			if (k == 4095) {
				if (fat_read_block(&fsb->sb, buf, i))
					return 0;
				fsb->buf_sector_base = i;
				k = j;
			}
		}
	}

	if (fsb->fat_type == FAT32)
		next = *((uint32_t *)&buf[k]) & 0x0fffffff;
	else if (fsb->fat_type == FAT16)
		next = *((uint16_t *)&buf[k]);
	else {
		next = u8_to_u16(buf[k], buf[k + 1]);
		if (current_clus & 0x0001)
			next >>= 4;
		else
			next &= 0x0fff;
	}

	return next;
}

static uint32_t fat16_get_next_data_block(struct fat_super_block *fsb, uint32_t current_clus)
{
	uint16_t count = fsb->fat12_16_root_dir_blk;

	if (current_clus > (0xfff7 - count) && current_clus != 0) {
		if ((--current_clus) == (0xfff7 - count))
			current_clus = 0;

		return current_clus;
	}

	if (current_clus == 0)
		return 0;

	return fat_get_next_data_block(fsb, current_clus);
}

static struct fnode *new_fat_fnode(struct fat_super_block *fsb,
		struct fat_fnode *parent, struct fat_dir_entry *entry)
{
	struct fat_fnode *ffnode;

	ffnode = kzalloc(sizeof(struct fat_fnode));
	if (!ffnode)
		return NULL;

	fat_fill_fnode(ffnode, fsb, entry);
	list_add_tail(&parent->fnode.child, &ffnode->fnode.list);
	
	return &ffnode->fnode;
}

static struct fnode *__fat_find_file(struct fat_super_block *fsb,
		struct fat_fnode *parent, char *buffer, char *name)
{
	struct fat_dir_entry *entry = (struct fat_dir_entry *)buffer;
	int len = strlen(name);
	char *long_name = &name[12];
	int i, is_long = len < 12 ? 0 : 1;

	for (i = 0; i < fsb->dentry_per_block; i++, entry++) {
		/*
		 * entry->name[0] == 0 means the directory is empty.
		 * need to check below check.
		 */
		if (fat_entry_is_long_name(entry) || entry->name[0] == 0)
			continue;

		if (is_long) {
			/* 
			 * we compare the min size of the short name
			 * when the file's name is long
			 */
			if (strncmp(name, entry->name, FAT_LONG_NAME_MIN_MATCH_SIZE))
				continue;

			/* 
			 * do not consider the case when the dir entry info is
			 * in different clus, to be done later
			 */
			if (!cmp_long_name(buffer, i, long_name, len - 11))
				goto find_file;

		} else {
			if (!strncmp(name, entry->name, 11))
				goto find_file;
		}
	}

	return NULL;

find_file:
	return new_fat_fnode(fsb, parent, entry);
}

static struct fnode *fat_find_file(struct fnode *parent, char *name)
{
	struct fat_fnode *ffnode = FNODE_TO_FAT_FNODE(parent);
	struct super_block *sb = parent->partition->sb;
	struct fat_super_block *fsb = SB_TO_FAT_SB(sb);
	uint32_t next_clus = ffnode->first_clus;
	struct fnode *fnode = NULL;
	char *buf;
	int ret;

	buf = get_pages(PAGE_NR(fsb->clus_size));
	if (!buf)
		return NULL;

	/*
	 * list for all cluster data.
	 */
	while (next_clus != 0) {
		ret = fat_read_block(sb, buf, next_clus);
		if (ret)
			break;

		fnode = __fat_find_file(fsb, ffnode, buf, name);
		if (fnode)
			break;

		if (fsb->fat_type == FAT32)
			next_clus = fat_get_next_data_block(fsb, next_clus);
		else
			next_clus = fat16_get_next_data_block(fsb, next_clus);
	}

	kfree(buf);

	return fnode;
}

static int get_file_name_type(char *name, int f, int b)
{
	/*
	 * support for unix hidden file, but windows
	 * do not support support it
	 */
	if (*name == '.')
		return FAT_NAME_TYPE_LONG;

	if (f > 8)
		return FAT_NAME_TYPE_LONG;

	if (f == 0) {
		if (b > 3)
			return FAT_NAME_TYPE_LONG;
		else
			return FAT_NAME_TYPE_SHORT;
	}

	if (f < 8) {
		if (b <= 3)
			return FAT_NAME_TYPE_SHORT;
		else
			return FAT_NAME_TYPE_LONG;
	}

	return FAT_NAME_TYPE_UNKNOWN;
}

static int is_lower(char ch)
{
	return ((ch <= 'z') && (ch >= 'a'));
}

static char lower_to_upper(char ch)
{
	if (is_lower(ch))
		return (ch -'a' + 'A');

	return ch;
}

static int fill_fat_name(char *target, char *source, int len, int type)
{
	int i = 0;
	int dot_pos = 0;
	char ch;

	/* skip the . at the begining of the string */
	while (source[dot_pos] == '.')
		dot_pos++;
	do {
		ch = lower_to_upper(source[dot_pos++]);
		if (ch == 0)
			break;

		if ((ch != ' ') && (ch != '.'))
			target[i++] = ch;

		if (dot_pos == len)
			break;
	} while (i < 6);

	/* 
	 * if the name is a long name file we add ~ 
	 * the end of the name and 0 will indicate
	 * we need to match the long name
	 */
	for (; i < 8; i++) {
		if (type) {
			target[i++] = '~';
			target[i] = '1';
			type = 0;
		} else {
			target[i] = ' ';
		}
	}

	return 0;
}

/* to ensure the following string: " er .txt" ".123.123" "egg .txt" "egg~.txt" */
static int fill_fat_appendix(char *target, char *source, int len, int type)
{
	int dot_pos = 0;
	int i = 0;
	char ch;

	if (!len)
		goto out;

	/*
	 * skip the . at the begining of the string and
	 * also need to skip the blank when
	 */
	while (source[dot_pos] == '.')
		dot_pos++;

	do {
		ch = lower_to_upper(source[dot_pos++]);
		if (ch == 0)
			break;

		if (ch != ' ') {
			target[8 + i] = ch;
			i++;
		}
				
		/* reach the end of the string */
		if (dot_pos == len)
			break;
	} while (i < 3);
	
out:	
	for (; i < 3; i++ )
		target[8 + i] = ' ';

	return 0;
}

static int fat_parse_name(char *name)
{
	char *buf;
	short *tmp1;
	int i, dot_pos, type = 0;
	int len = strlen(name);
	int f, b;

	if (!name || !len)
		return -EINVAL;

	buf = get_page();
	if (!buf)
		return -EAGAIN;

	for (dot_pos = len; dot_pos > 0; dot_pos--) {
		if (name[dot_pos] == '.')
			break;
	}

	f = dot_pos;
	if (f == 0)
		f = len;

	if (dot_pos == 0)
		b = 0;
	else
		b = len - dot_pos - 1;

	type = get_file_name_type(name, f, b);
	pr_debug("dot_pos:%d f:%d b:%d type:%d\n",
			dot_pos, f, b, type);

	/* copy the name to the buffer dot_pos will */
	memset(buf, 0, 512);

	/* add 1 to skip the dot '.' */
	fill_fat_appendix(buf, name + f + 1, b, type);
	fill_fat_name(buf, name, f, type);

	/* covert the string to unicode */
	tmp1 = (short *)(&buf[12]);
	for (i = 0; i < len; i++)
		tmp1[i] = name[i];
		
	buf[11] = type;
	memcpy(name, buf, 512);
	free_pages(buf);

	return 0;
}

static ssize_t fat_read(struct fnode *fnode, char *buf,
		size_t size, off_t offset)
{
	return 0;
}

static ssize_t fat_write(struct fnode *fnode, char *buf,
		size_t size, off_t offset)
{
	return 0;
}

static int fat_match(int type, char *name)
{
	switch (type) {
	case 01:
	case 04:
	case 05:
	case 06:
	case 07:
	case 0x0b:
	case 0xc:
	case 0xe:
	case 0x1b:
	case 0x1c:
	case 0x1e:
		return 1;
	default:
		return 0;
	}
}

hidden struct filesystem fat_fs = {
	.name			= "msdos",
	.match			= fat_match,
	.find_file		= fat_find_file,
	.read			= fat_read,
	.write			= fat_write,
	.create_super_block	= fat_create_super_block,
};
