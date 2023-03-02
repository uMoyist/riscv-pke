/*
 * RFS (Ramdisk File System) is a customized simple file system installed in the
 * RAM disk. added @lab4_1.
 * Layout of the file system:
 *
 * ******** RFS MEM LAYOUT (112 BLOCKS) ****************
 *   superblock  |  disk inodes  |  bitmap  |  free blocks  *
 *     1 block   |   10 blocks   |     1    |     100       *
 * *****************************************************
 *
 * The disk layout of rfs is similar to the fs in xv6.
 */
#include "rfs.h"

#include "pmm.h"
#include "ramdev.h"
#include "spike_interface/spike_utils.h"
#include "util/string.h"
#include "vfs.h"

/**** vinode inteface ****/
const struct vinode_ops rfs_i_ops = {
    .viop_read = rfs_read,
    .viop_write = rfs_write,
    .viop_create = rfs_create,
    .viop_lseek = rfs_lseek,
    .viop_disk_stat = rfs_disk_stat,
    .viop_lookup = rfs_lookup,

    .viop_write_back_vinode = rfs_write_back_vinode,
};

/**** rfs utility functions ****/
//
// register rfs to the fs list supported by PKE.
//
int register_rfs() {
  struct file_system_type *fs_type = (struct file_system_type *)alloc_page();
  fs_type->type_num = RFS_TYPE;
  fs_type->get_superblock = rfs_get_superblock;

  for (int i = 0; i < MAX_SUPPORTED_FS; i++) {
    if (fs_list[i] == NULL) {
      fs_list[i] = fs_type;
      return 0;
    }
  }
  return -1;
}

//
// format "dev" with rfs. note the "dev" should be a ram disk device.
//
int rfs_format_dev(struct device *dev) {
  struct rfs_device *rdev = rfs_device_list[dev->dev_id];

  // ** first, format the superblock
  // build a new superblock
  struct super_block *super = (struct super_block *)rdev->iobuffer;
  super->magic = RFS_MAGIC;
  super->size =
      1 + RFS_MAX_INODE_BLKNUM + 1 + RFS_MAX_INODE_BLKNUM * RFS_DIRECT_BLKNUM;
  // only direct index blocks
  super->nblocks = RFS_MAX_INODE_BLKNUM * RFS_DIRECT_BLKNUM;
  super->ninodes = RFS_BLKSIZE / RFS_INODESIZE * RFS_MAX_INODE_BLKNUM;

  // write the superblock to RAM Disk0
  if (rfs_w1block(rdev, RFS_BLK_OFFSET_SUPER) != 0)  // write to device
    panic("RFS: failed to write superblock!\n");

  // ** second, set up the inodes and write them to RAM disk
  // build an empty inode disk block which has RFS_BLKSIZE/RFS_INODESIZE(=32)
  // disk inodes
  struct rfs_dinode *p_dinode = (struct rfs_dinode *)rdev->iobuffer;
  for (int i = 0; i < RFS_BLKSIZE / RFS_INODESIZE; ++i) {
    p_dinode->size = 0;
    p_dinode->type = R_FREE;
    p_dinode->nlinks = 0;
    p_dinode->blocks = 0;
    p_dinode = (struct rfs_dinode *)((char *)p_dinode + RFS_INODESIZE);
  }

  // write RFS_MAX_INODE_BLKNUM(=10) empty inode disk blocks to RAM Disk0
  for (int inode_block = 0; inode_block < RFS_MAX_INODE_BLKNUM; ++inode_block) {
    if (rfs_w1block(rdev, RFS_BLK_OFFSET_INODE + inode_block) != 0)
      panic("RFS: failed to initialize empty inodes!\n");
  }

  // build root directory inode (ino = 0)
  struct rfs_dinode root_dinode;
  root_dinode.size = 0;
  root_dinode.type = R_DIR;
  root_dinode.nlinks = 1;
  root_dinode.blocks = 1;
  root_dinode.addrs[0] = RFS_BLK_OFFSET_FREE;

  // write root directory inode to RAM Disk0 (ino = 0)
  if (rfs_write_dinode(rdev, &root_dinode, 0) != 0) {
    sprint("RFS: failed to write root inode!\n");
    return -1;
  }

  // ** third, write freemap to disk
  int *freemap = (int *)rdev->iobuffer;
  memset(freemap, 0, RFS_BLKSIZE);
  freemap[0] = 1;  // the first data block is used for root directory

  // write the bitmap to RAM Disk0
  if (rfs_w1block(rdev, RFS_BLK_OFFSET_BITMAP) != 0) {  // write to device
    sprint("RFS: failed to write bitmap!\n");
    return -1;
  }

  sprint("RFS: format %s done!\n", dev->dev_name);
  return 0;
}

// ** Note: If you use the following four functions interchangeably,
// ** be sure to watch out for IOBUFFER BEING OVERWRITTEN !!!

//
// call ramdisk_read via the device structure.
// read the "n_block"^th block from RAM disk to the iobuffer of rfs_dev.
//
int rfs_r1block(struct rfs_device *rfs_dev, int n_block) {
  return dop_read(rfs_dev, n_block);
}

//
// call ramdisk_write via the device structure.
// write iobuffer of rfs_dev to RAM disk at the "n_block"^th block.
//
int rfs_w1block(struct rfs_device *rfs_dev, int n_block) {
  return dop_write(rfs_dev, n_block);
}

//
// read disk inode from RAM disk
//
struct rfs_dinode *rfs_read_dinode(struct rfs_device *rdev, int n_inode) {
  int n_block = n_inode / (RFS_BLKSIZE / RFS_INODESIZE) + RFS_BLK_OFFSET_INODE;
  int offset = n_inode % (RFS_BLKSIZE / RFS_INODESIZE);

  // call ramdisk_read defined in dev.c
  if (dop_read(rdev, n_block) != 0) return NULL;
  struct rfs_dinode *dinode = (struct rfs_dinode *)alloc_page();
  memcpy(dinode, (char *)rdev->iobuffer + offset * RFS_INODESIZE,
         sizeof(struct rfs_dinode));
  return dinode;
}

//
// write disk inode to RAM disk.
// note: we need first read the "disk" block containing the "n_inode"^th inode,
// modify it, and write the block back to "disk" eventually.
//
int rfs_write_dinode(struct rfs_device *rdev, const struct rfs_dinode *dinode,
                     int n_inode) {
  int n_block = n_inode / (RFS_BLKSIZE / RFS_INODESIZE) + RFS_BLK_OFFSET_INODE;
  int offset = n_inode % (RFS_BLKSIZE / RFS_INODESIZE);

  // call ramdisk_read defined in dev.c
  dop_read(rdev, n_block);
  memcpy(rdev->iobuffer + offset * RFS_INODESIZE, dinode,
         sizeof(struct rfs_dinode));
  // call ramdisk_write defined in dev.c
  int ret = dop_write(rdev, n_block);

  return ret;
}

//
// allocate a block from RAM disk
//
int rfs_alloc_block(struct super_block *sb) {
  int free_block = -1;
  // think of s_fs_info as freemap information
  int *freemap = (int *)sb->s_fs_info;
  for (int block = 0; block < sb->nblocks; ++block) {
    if (freemap[block] == 0) {  // find a free block
      freemap[block] = 1;
      free_block = RFS_BLK_OFFSET_FREE + block;
      break;
    }
  }
  if (free_block == -1) panic("rfs_alloc_block: no more free block!\n");
  return free_block;
}

//
// free a block in RAM disk
//
int rfs_free_block(struct super_block *sb, int block_num) {
  int *freemap = (int *)sb->s_fs_info;
  freemap[block_num - RFS_BLK_OFFSET_FREE] = 0;
  return 0;
}

//
// add a new directory entry to a directory
//
int rfs_add_direntry(struct vinode *dir, const char *name, int inum) {
  if (dir->type != DIR_I) {
    sprint("rfs_add_direntry: not a directory!\n");
    return -1;
  }

  struct rfs_device *rdev = rfs_device_list[dir->sb->s_dev->dev_id];
  int n_block = dir->addrs[dir->size / RFS_BLKSIZE];
  if (rfs_r1block(rdev, n_block) != 0) {
    sprint("rfs_add_direntry: failed to read block %d!\n", n_block);
    return -1;
  }

  // prepare iobuffer
  char *addr = (char *)rdev->iobuffer + dir->size % RFS_BLKSIZE;
  struct rfs_direntry *p_direntry = (struct rfs_direntry *)addr;
  p_direntry->inum = inum;
  strcpy(p_direntry->name, name);

  // write the modified (parent) directory block back to disk
  if (rfs_w1block(rdev, n_block) != 0) {
    sprint("rfs_add_direntry: failed to write block %d!\n", n_block);
    return -1;
  }

  // update its parent dir state
  dir->size += sizeof(struct rfs_direntry);

  // write the parent dir inode back to disk
  if (rfs_write_back_vinode(dir) != 0) {
    sprint("rfs_add_direntry: failed to write back parent dir inode!\n");
    return -1;
  }

  return 0;
}

//
// alloc a new (and empty) vinode
//
struct vinode *rfs_alloc_vinode(struct super_block *sb) {
  struct vinode *vinode = default_alloc_vinode(sb);
  vinode->i_ops = &rfs_i_ops;
  return vinode;
}

//
// convert vfs inode to disk inode, and write it back to disk
//
int rfs_write_back_vinode(struct vinode *vinode) {
  // copy vinode info to disk inode
  struct rfs_dinode dinode;
  dinode.size = vinode->size;
  dinode.nlinks = vinode->nlinks;
  dinode.blocks = vinode->blocks;
  dinode.type = vinode->type;
  for (int i = 0; i < RFS_DIRECT_BLKNUM; ++i) {
    dinode.addrs[i] = vinode->addrs[i];
  }

  struct rfs_device *rdev = rfs_device_list[vinode->sb->s_dev->dev_id];
  if (rfs_write_dinode(rdev, &dinode, vinode->inum) != 0) {
    sprint("rfs_free_write_back_inode: failed to write back disk inode!\n");
    return -1;
  }

  return 0;
}

//
// update vinode info by reading disk inode
//
int rfs_update_vinode(struct vinode *vinode) {
  struct rfs_device *rdev = rfs_device_list[vinode->sb->s_dev->dev_id];
  struct rfs_dinode *dinode = rfs_read_dinode(rdev, vinode->inum);
  if (dinode == NULL) {
    sprint("rfs_update_vinode: failed to read disk inode!\n");
    return -1;
  }
  vinode->size = dinode->size;
  vinode->nlinks = dinode->nlinks;
  vinode->blocks = dinode->blocks;
  vinode->type = dinode->type;
  for (int i = 0; i < RFS_DIRECT_BLKNUM; ++i) {
    vinode->addrs[i] = dinode->addrs[i];
  }
  free_page(dinode);

  return 0;
}

/**** vfs-rfs file interface functions ****/
//
// read the content (for "len") of a file ("f_inode"), and copy the content
// to "r_buf".
//
ssize_t rfs_read(struct vinode *f_inode, char *r_buf, ssize_t len,
                 int *offset) {
  // obtain disk inode from vfs inode
  if (f_inode->size < *offset)
    panic("rfs_read:offset should less than file size!");

  if (f_inode->size < (*offset + len)) len = f_inode->size - *offset;

  char buffer[len + 1];

  // compute how many blocks we need to read
  int align = *offset % RFS_BLKSIZE;
  int block_offset = *offset / RFS_BLKSIZE;
  int buf_offset = 0;

  int readtimes = (align + len) / RFS_BLKSIZE;
  int remain = (align + len) % RFS_BLKSIZE;

  struct rfs_device *rdev = rfs_device_list[f_inode->sb->s_dev->dev_id];

  // read first block
  rfs_r1block(rdev, f_inode->addrs[block_offset]);
  int first_block_len = (readtimes == 0 ? len : RFS_BLKSIZE - align);
  memcpy(buffer + buf_offset, rdev->iobuffer + align, first_block_len);
  buf_offset += first_block_len;
  block_offset++;
  readtimes--;

  // readtimes < 0 means that the file has only one block (and not full),
  // so our work is done
  // otherwise...
  if (readtimes >= 0) {
    // read in complete blocks
    while (readtimes != 0) {
      rfs_r1block(rdev, f_inode->addrs[block_offset]);
      memcpy(buffer + buf_offset, rdev->iobuffer, RFS_BLKSIZE);
      buf_offset += RFS_BLKSIZE;
      block_offset++;
      readtimes--;
    }

    // read in the remaining data
    if (remain > 0) {
      rfs_r1block(rdev, f_inode->addrs[block_offset]);
      memcpy(buffer + buf_offset, rdev->iobuffer, remain);
    }
  }

  buffer[len] = '\0';
  strcpy(r_buf, buffer);

  *offset += len;
  return len;
}

//
// write the content of "w_buf" (lengthed "len") to a file ("f_inode").
//
ssize_t rfs_write(struct vinode *f_inode, const char *w_buf, ssize_t len,
                  int *offset) {
  if (f_inode->size < *offset) {
    panic("rfs_write:offset should less than file size!");
  }

  // compute how many blocks we need to write
  int align = *offset % RFS_BLKSIZE;
  int writetimes = (len + align) / RFS_BLKSIZE;
  int remain = (len + align) % RFS_BLKSIZE;

  int buf_offset = 0;
  int block_offset = *offset / RFS_BLKSIZE;

  struct rfs_device *rdev = rfs_device_list[f_inode->sb->s_dev->dev_id];

  // write first block
  if (align != 0) {
    rfs_r1block(rdev, f_inode->addrs[block_offset]);
    int first_block_len = (writetimes == 0 ? len : RFS_BLKSIZE - align);
    memcpy(rdev->iobuffer + align, w_buf, first_block_len);
    rfs_w1block(rdev, f_inode->addrs[block_offset]);

    buf_offset += first_block_len;
    block_offset++;
    writetimes--;
  }

  // writetimes < 0 means that the file has only one block (and not full),
  // so our work is done
  // otherwise...
  if (writetimes >= 0) {
    // write complete blocks
    while (writetimes != 0) {
      if (block_offset == f_inode->blocks) {  // need to create new block
        // allocate a free block for the file
        f_inode->addrs[block_offset] = rfs_alloc_block(f_inode->sb);
        f_inode->blocks++;
      }

      memcpy(rdev->iobuffer, w_buf + buf_offset, RFS_BLKSIZE);
      rfs_w1block(rdev, f_inode->addrs[block_offset]);

      buf_offset += RFS_BLKSIZE;
      block_offset++;
      writetimes--;
    }

    // write the remaining data
    if (remain > 0) {
      if (block_offset == f_inode->blocks) {
        f_inode->addrs[block_offset] = rfs_alloc_block(f_inode->sb);
        ++f_inode->blocks;
      }
      memcpy(rdev->iobuffer, w_buf + buf_offset, remain);
      rfs_w1block(rdev, f_inode->addrs[block_offset]);
    }
  }

  // update file size
  f_inode->size =
      (f_inode->size < *offset + len ? *offset + len : f_inode->size);

  *offset += len;
  return len;
}

//
// lookup a directory entry("sub_dentry") under "parent".
// note that this is a one level lookup ,and the vfs layer will call this
// function several times until the final file is found.
// return: if found, return its vinode, otherwise return NULL
//
struct vinode *rfs_lookup(struct vinode *parent, struct dentry *sub_dentry) {
  struct rfs_direntry *p_direntry = NULL;
  struct vinode *child_vinode = NULL;

  int total_direntrys = parent->size / sizeof(struct rfs_direntry);
  int one_block_direntrys = RFS_BLKSIZE / sizeof(struct rfs_direntry);

  struct rfs_device *rdev = rfs_device_list[parent->sb->s_dev->dev_id];

  // browse the dir entries contained in a directory file
  for (int i = 0; i < total_direntrys; ++i) {
    if (i % one_block_direntrys == 0) {  // read in the disk block at boundary
      rfs_r1block(rdev, parent->addrs[i / one_block_direntrys]);
      p_direntry = (struct rfs_direntry *)rdev->iobuffer;
    }
    if (strcmp(p_direntry->name, sub_dentry->name) == 0) {  // found
      child_vinode = rfs_alloc_vinode(parent->sb);
      child_vinode->inum = p_direntry->inum;
      if (rfs_update_vinode(child_vinode) != 0)
        panic("rfs_lookup: read inode failed!");
      break;
    }
    ++p_direntry;
  }
  return child_vinode;
}

//
// create a file with "sub_dentry->name" at directory "parent" in rfs.
// return the vfs inode of the file being created.
//
struct vinode *rfs_create(struct vinode *parent, struct dentry *sub_dentry) {
  struct rfs_device *rdev = rfs_device_list[parent->sb->s_dev->dev_id];

  // ** find a free disk inode to store the file that is going to be created
  struct rfs_dinode *free_dinode = NULL;
  int free_inum = 0;
  for (int i = 0; i < (RFS_BLKSIZE / RFS_INODESIZE * RFS_MAX_INODE_BLKNUM);
       ++i) {
    free_dinode = rfs_read_dinode(rdev, i);
    if (free_dinode->type == R_FREE) {  // found
      free_inum = i;
      break;
    }
    free_page(free_dinode);
  }

  if (free_dinode == NULL)
    panic("rfs_create: no more free disk inode, we cannot create file.\n" );

  // initialize the states of the file being created

  // TODO (lab4_1): implement the code for populating the disk inode (free_dinode) 
  // of a new file being created.
  // hint:  members of free_dinode to be filled are:
  // size, should be zero for a new file.
  // type, see kernel/rfs.h and find the type for a rfs file.
  // nlinks, i.e., the number of links.
  // blocks, i.e., its block count.
  // Note: DO NOT DELETE CODE BELOW PANIC.
  // panic("You need to implement the code of populating a disk inode in lab4_1.\n" );
  free_dinode->size = 0;
  free_dinode->blocks = 1;
  free_dinode->nlinks = 1;
  free_dinode->type = R_FILE;

  // DO NOT REMOVE ANY CODE BELOW.
  // allocate a free block for the file
  free_dinode->addrs[0] = rfs_alloc_block(parent->sb);

  // **  write the disk inode of file being created to disk
  rfs_write_dinode(rdev, free_dinode, free_inum);
  free_page(free_dinode);

  // ** build vfs inode according to dinode
  struct vinode *new_vinode = rfs_alloc_vinode(parent->sb);
  new_vinode->inum = free_inum;
  rfs_update_vinode(new_vinode);

  // ** append the new file as a direntry to its parent dir
  int result = rfs_add_direntry(parent, sub_dentry->name, free_inum);
  if (result == -1) {
    sprint("rfs_create: rfs_add_direntry failed");
    return NULL;
  }

  return new_vinode;
}

//
// there are two types of seek (specify by whence): LSEEK_SET, SEEK_CUR
// LSEEK_SET: set the file pointer to the offset
// LSEEK_CUR: set the file pointer to the current offset plus the offset
// return 0 if success, otherwise return -1
//
int rfs_lseek(struct vinode *f_inode, ssize_t new_offset, int whence, int *offset) {
  int file_size = f_inode->size;

  switch (whence) {
    case LSEEK_SET:
      if (new_offset < 0 || new_offset > file_size) {
        sprint("rfs_lseek: invalid offset!\n");
        return -1;
      }
      *offset = new_offset;
      break;
    case LSEEK_CUR:
      if (*offset + new_offset < 0 || *offset + new_offset > file_size) {
        sprint("rfs_lseek: invalid offset!\n");
        return -1;
      }
      *offset += new_offset;
      break;
    default:
      sprint("rfs_lseek: invalid whence!\n");
      return -1;
  }
  
  return 0;
}

//
//  read disk inode information from disk
//
int rfs_disk_stat(struct vinode *vinode, struct istat *istat) {
  struct rfs_device *rdev = rfs_device_list[vinode->sb->s_dev->dev_id];
  struct rfs_dinode *dinode = rfs_read_dinode(rdev, vinode->inum);
  if (dinode == NULL) {
    sprint("rfs_disk_stat: read dinode failed!\n");
    return -1;
  }

  istat->st_inum = 1;
  istat->st_inum = vinode->inum;  // get inode number from vinode

  istat->st_size = dinode->size;
  istat->st_type = dinode->type;
  istat->st_nlinks = dinode->nlinks;
  istat->st_blocks = dinode->blocks;
  free_page(dinode);
  return 0;
}

/**** vfs-rfs file system type interface functions ****/
struct super_block *rfs_get_superblock(struct device *dev) {
  struct rfs_device *rdev = rfs_device_list[dev->dev_id];

  // read super block from ramdisk
  if (rfs_r1block(rdev, RFS_BLK_OFFSET_SUPER) != 0)
    panic("RFS: failed to read superblock!\n");

  struct rfs_superblock d_sb;
  memcpy(&d_sb, rdev->iobuffer, sizeof(struct rfs_superblock));

  // set the data for the vfs super block
  struct super_block *sb = alloc_page();
  sb->magic = d_sb.magic;
  sb->size = d_sb.size;
  sb->nblocks = d_sb.nblocks;
  sb->ninodes = d_sb.ninodes;
  sb->s_dev = dev;

  if( sb->magic != RFS_MAGIC ) 
    panic("rfs_get_superblock: wrong ramdisk device!\n");

  // build root dentry and root inode
  struct vinode *root_inode = rfs_alloc_vinode(sb);
  root_inode->inum = 0;
  rfs_update_vinode(root_inode);

  struct dentry *root_dentry = alloc_vfs_dentry("/", root_inode, NULL);
  sb->s_root = root_dentry;

  // save the bitmap in the s_fs_info field
  if (rfs_r1block(rdev, RFS_BLK_OFFSET_BITMAP) != 0)
    panic("RFS: failed to read bitmap!\n");
  void *bitmap = alloc_page();
  memcpy(bitmap, rdev->iobuffer, RFS_BLKSIZE);
  sb->s_fs_info = bitmap;

  return sb;
}
