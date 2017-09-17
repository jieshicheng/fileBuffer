#include "direct.h"
#include "ide.h"
#include "stdint.h"
#include "memory.h"
#include "global.h"
#include "fs.h"
#include "debug.h"
#include "string.h"
#include "inode.h"
#include "file.h"


extern struct partition *cur_part;

struct dir root_dir;

/**
 *	打开根目录，不可关闭
 */
void open_root_dir(struct partition *part)
{
	root_dir.inode = inode_open(part, part->sb->root_inode_no);
	root_dir.dir_pos = 0;
}


/**
 *	打开目录接口。指定inode编号，打开这个inode并以目录结构返回
 */
struct dir *dir_open(struct partition *part, uint32_t inode_no)
{
	struct dir *pdir = (struct dir *)sys_malloc(sizeof(struct dir));
	pdir->inode = inode_open(part, inode_no);
	pdir->dir_pos = 0;
	return pdir;
}


/**
 *	在目录中寻找指定名字的目录项，成功则true
 */
enum bool search_dir_entry(struct partition *part, struct dir *pdir, const char *name, struct dir *dir_e)
{
	uint32_t block_cnt = 140;
	uint32_t *all_blocks = (uint32_t *)sys_malloc(48 + 512);
	if( all_blocks == NULL ) {
		printk("malloc memory failed\n");
		return false;
	}
	uint32_t block_idx = 0;
	while( block_idx < 12 ) {
		all_blocks[block_idx] = pdir->inode->i_sectors[block_idx];
		block_idx++;
	}
	if( pdir->inode->i_sectors[12] != 0 ) {
		ide_read(part->my_disk, pdir->inode->i_sectors[12], all_blocks + 12, 1);
	}
	uint8_t *buf = (uint8_t *)sys_malloc(SECTOR_SIZE);
	struct dir_entry *p_de = (struct dir_entry *)buf;
	uint32_t dir_entry_size = part->sb->dir_entry_size;
	uint32_t dir_entry_cnt = SECTOR_SIZE / dir_entry_size;
	while( block_idx < block_cnt ) {
		if( all_blocks[block_idx] == 0 ) {
			block_idx++;
			continue;
		}
		ide_read(part->my_disk, all_blocks[block_idx], buf ,1);
		uint32_t dir_entry_idx = 0;
		while( dir_entry_idx < dir_entry_cnt ) {
			if( !strcmp(p_de->name, name) ) {
				memcpy(dir_e, p_de, dir_entry_size);
				sys_free(buf);
				sys_free(all_blocks);
				return true;
			}
			dir_entry_idx++;
			p_de++;
		}
		block_idx++;
		p_de = (struct dir_entry *)buf;
		memset(buf, 0, SECTOR_SIZE);
	}
	sys_free(buf);
	sys_free(all_blocks);
	return false;
}

/**
 *	关闭目录，通过指定目录指针
 */
void dir_close(struct dir *dir)
{
	if( dir == &root_dir )
		return;
	inode_close(dir->inode);
	sys_free(dir);
}

/**
 *	创造目录项。并将数据放到参数p_de中
 */
void create_dir_entry(char *name, uint32_t inode_no, uint8_t file_type, struct dir_entry *p_de)
{
	ASSERT(strlen(name) <= MAX_FILE_NAME_LEN);
	memcpy(p_de->name, name, strlen(name));
	p_de->i_no = inode_no;
	p_de->f_type = file_type;
}

/**
 *	将一个目录项保存到父目录中。
 */
enum bool sync_dir_entry(struct dir *parent_dir, struct dir_entry *p_de, void *io_buf)
{
	struct inode *dir_inode = parent_dir->inode;
	uint32_t dir_size = dir_inode->i_size;
	uint32_t dir_entry_size = cur_part->sb->dir_entry_size;

	ASSERT(dir_size % dir_entry_size == 0);
	uint32_t dir_entrys_per_sec = (512 / dir_entry_size);
	int32_t block_lba = -1;
	uint8_t block_idx = 0;
	uint32_t all_blocks[140] = {0};

	while( block_idx < 12 ) {
		all_blocks[block_idx] = dir_inode->i_sectors[block_idx];
		block_idx++;
	}

	struct dir_entry *dir_e = (struct dir_entry *)io_buf;
	int32_t block_bitmap_idx = -1;

	block_idx = 0;
	while( block_idx < 140 ) {
		block_bitmap_idx = -1;
		if( all_blocks[block_idx] == 0 ) {
			block_lba = block_bitmap_alloc(cur_part);
			if( block_lba == -1 ) {
				printk("alloc block bitmap failed\n");
				return false;
			}
			block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
			ASSERT(block_bitmap_idx != -1);
			bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

			block_bitmap_idx = -1;
			if( block_idx < 12 ) {
				dir_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;
			}
			else if( block_idx == 12 ) {
				dir_inode->i_sectors[12] = block_lba;
				block_lba = -1;
				block_lba = block_bitmap_alloc(cur_part);
				if( block_lba == -1 ) {
					block_bitmap_idx = dir_inode->i_sectors[12] - cur_part->sb->data_start_lba;
					bitmap_set(&cur_part->block_bitmap, block_bitmap_idx, 0);
					dir_inode->i_sectors[12] = 0;
					printk("alloc block bitmap failed\n");
					return false;
				}
				block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
				ASSERT(block_bitmap_idx != -1);
				bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

				all_blocks[12] = block_lba;
				ide_write(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
			}
			else {
				all_blocks[block_idx] = block_lba;
				ide_write(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
			}
			memset(io_buf, 0, 512);
			memcpy(io_buf, p_de, dir_entry_size);
			ide_write(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
			dir_inode->i_size += dir_entry_size;
			return true;
		}

		ide_read(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
		uint8_t dir_entry_dix = 0;
		while( dir_entry_idx < dir_entrys_per_sec ) {
			if( (dir_e + dir_entry_idx)->f_type == FT_UNKNOWN ) {
				memcpy(dir_e + dir_entry_idx, p_de, dir_entry_size);
				ide_write(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
				dir_inode->i_size += dir_entry_size;
				return true;
			}
			dir_entry_idx++;
		}
		block_idx++;
	}
	printk("directory is full\n");
	return false;
}


























