#define FUSE_USE_VERSION 26
#define _XOPEN_SOURCE
#include <string.h>
#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_common.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

FILE* image; 
char *filebuf;
int curBlock;
int size = 512;
unsigned char block[512];
unsigned char fat[512];

//returns the index of the file directory entry. And now the array block[512] now holds the Directory block that holds that entry.
int find(const char* path) {
	
	int numBlock = 253, counter, num;
	char filename[13];
	char *slash;

	//looks for the path, if it exists then it will do nothing, if not then return error.
	while (numBlock >= 240) {
		fseek(image, size * numBlock,SEEK_SET);
		if((num = fread(block, 1, sizeof(block), image)) == size) {
			counter = 4;		
			while (counter < 512) {		
				if (block[counter] == 0x00) {
					counter = counter + 32;
				}
				else {	
					memcpy(filename, block + counter, 12);
					filename[12] = '\0';
					slash = strrchr(path, '/');
					if(strncmp(filename, slash + 1, 11) == 0) {
						curBlock = numBlock;
						return counter - 4;
					}
					counter = counter + 32;
				}
			}
			numBlock--;
		}	
	}
	return -1;
}

int setFat() {
	int fatIndex = 254;
	fseek(image, size * fatIndex, SEEK_SET);
	if (fread(fat, 1, sizeof(fat), image) == size) {
		return 0;
	}
	return -1;
}

int findEmpty() {
	int index,i = 239;
	char blockIndex[2];	
	setFat();
	memcpy(blockIndex, fat + i,  2);
	index = blockIndex[1]*0x100 + blockIndex[0];
	while (index != 65532) {
		i = i - 2;
		memcpy(blockIndex, fat + i,  2);
		index = blockIndex[1]*0x100 + blockIndex[0];
	}
	return i;
}

unsigned char formatBCD(char* timestamp, int d1, int d2) {
	char buf[3];
	char entry;
	int data;

	buf[0] = timestamp[d1];
	buf[1] = timestamp[d2];
	buf[2] = '\0';
	data = atoi(buf);
	if (d2 == 15) {
		data = data - 1;
	}
	entry = ((data / 10) << 4 | (data % 10));
	return entry;
}

static int vmufs_getattr(const char* path, struct stat* stbuf) {
	memset(stbuf, 0, sizeof(struct stat));
	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0775;
		stbuf->st_mtime = time(NULL);
		stbuf->st_nlink = 2;
		return 0;	
	}
	else {	
//		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_mode = S_IFREG | S_IRWXU | S_IRWXG ;
		stbuf->st_nlink = 1;
		//looking for the file and then add the stats based on the directory information
		int counter;
		char timemod[8];
		if ((counter = find(path)) >= 0) {
			//set up for the time
			struct tm t = {0};
			time_t tm;		
			memcpy(timemod, block + counter + 16, 8);	
			t.tm_year = (((timemod[0] >> 4) * 10 + (timemod[0] & 15))* 100) + ((timemod[1] >> 4) * 10 + (timemod[1] & 15));
			t.tm_year = t.tm_year - 1900;
			t.tm_mon = (timemod[2] >> 4) * 10 + (timemod[2] & 15) - 1;
			t.tm_mday = (timemod[3] >> 4) * 10 + (timemod[3] & 15);
			t.tm_hour = (timemod[4] >> 4) * 10 + (timemod[4] & 15);
			t.tm_min = (timemod[5] >> 4) * 10 + (timemod[5] & 15);
			t.tm_sec = (timemod[6] >> 4) * 10 + (timemod[6] & 15) - 1;
			t.tm_wday = (timemod[7] >> 4) * 10 + (timemod[7] & 15) + 1;
			if (t.tm_wday > 6) {
				t.tm_wday = 0;
			}
			tm = mktime(&t);
			//set up the stats
			stbuf->st_size = block[counter + 24] * 512;
			stbuf->st_atime = time(NULL);
			stbuf->st_mtime = tm;
			stbuf->st_blksize = 512;
			stbuf->st_blocks = block[counter + 24];
		}		
		return 0;
	}
	return -ENOENT;	
}

static int vmufs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
	
	(void) offset;
	(void) fi;
	/*if (strcmp(path, "/") != 0) {
		return -ENOENT;
	}*/

	int numBlock = 253, counter, num;
	char filename[13];
	//look for the files and use filler
	while (numBlock >= 240) {
		fseek(image, size * numBlock,SEEK_SET);
		if((num = fread(block, 1, sizeof(block), image)) == size) {
			counter = 4;		
			while (counter < 512) {	
				if (block[counter] == 0x00) {
					counter = counter + 32;
				}
				else {		
					memcpy(filename, block + counter, 12 * sizeof(unsigned char));
					filename[12] = '\0';
					counter = counter + 32;
					filler(buf, filename, NULL, 0);	
				}
			}
			numBlock--;
		}	
	}
	return 0;
}

static int vmufs_open(const char* path, struct fuse_file_info* fi) {
	if ((fi->flags & 3) != O_RDONLY) {
		return -EACCES;
	}
	if (find(path) >= 0) {
		return 0;
	}
	return -ENOENT;
}

static int vmufs_read(const char* path, char *buf, size_t r_size, off_t offset, struct fuse_file_info* fi) {
	(void) fi;	

	int counter, index, sizeFile, modsize, total = 0;
	unsigned char blockIndex[2];
	setFat();
	if ((counter = find(path)) >= 0) {
		sizeFile = block[counter + 24] * size;
		if(offset > sizeFile) {
			return 0;
		}
		memcpy(blockIndex, block + counter + 2, 2);
		index = blockIndex[1]*0x100 + blockIndex[0];
		if (index < 0) {
			return 0;
		}
		//stops if written enough number of bytes
		while(r_size > 0) {
			//loads the contents of the first block
			fseek(image, size * index, SEEK_SET);
			fread(block, 1, sizeof(block), image);		
			if( r_size > 512) {
				modsize = 512;
			}
			else {
				modsize = r_size;
			}	
			//writes to the buffer
			if (total >= offset) {					
				memcpy(buf + (total - offset), block, modsize);
			}				
			total = total + modsize;
			r_size = r_size - modsize;
			
			//check the fat to get next block
			memcpy(blockIndex, fat + (index * 2), 2);
			index = blockIndex[1]*0x100 + blockIndex[0];
			//stops if end of chain marker		
			if (index >= 65530) {
				return total;
			}			
		}
		return total;	
	}
	return -ENOENT;
}

static int vmufs_write(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
	(void) fi;	

	int counter, index, sizeFile, modsize,i, total = 0;
	unsigned char blockIndex[2];
	setFat();
	if ((counter = find(path)) >= 0) {
		sizeFile = block[counter + 24] * size;
		memcpy(blockIndex, block + counter + 2, 2);
		index = blockIndex[1]*0x100 + blockIndex[0];
		//had no data before		
		if (index < 0) {
			while (size > 0) {					
				if(size > 512) {
					modsize = 512;
				}
				else { 
					modsize = size;
				}
				//writes the need bytes taking into account of remaining offset
				if (offset == 0) {
					memcpy(block, buf + total, modsize);
				}
				else {
					memcpy(block + offset, buf + total, (modsize - offset));
					offset = 0;
				}
				memcpy(fat + (index * 2), findEmpty, 2);			
				index = findEmpty();	
				fseek(image, size * index, SEEK_SET);
				fwrite(block, sizeof(unsigned char), sizeof(block), image);
				size = size - modsize;
				total = total + modsize;
			}
			fat[index * 2] = 0xFF;
			fat[index * 2 + 1] = 0xFC;
			return total;
		}
		//write for appending
		if(offset > sizeFile) {
			//finds the end of the file	
			while (index < 65530) {
				memcpy(blockIndex, fat + (index * 2), 2);
				index = blockIndex[1]*0x100 + blockIndex[0];
			}
			//add the padding if the offset is greater tahn the file length
			while (offset > 512) {
				for(i = 0; i < 512; i++) {
					block[i] = 0x00;
				}
				memcpy(fat + (index * 2), findEmpty, 2);			
				index = findEmpty();	
				fseek(image, size * index, SEEK_SET);
				fwrite(block, sizeof(unsigned char), sizeof(block), image);
				offset = offset - 512;
			}
			while (size > 0) {					
				if(size > 512) {
					modsize = 512;
				}
				else { 
					modsize = size;
				}
				//writes the need bytes taking into account of remaining offset
				if (offset == 0) {
					memcpy(block, buf + total, modsize);
				}
				else {
					memcpy(block + offset, buf + total, (modsize - offset));
					offset = 0;
				}
				memcpy(fat + (index * 2), findEmpty, 2);			
				index = findEmpty();	
				fseek(image, size * index, SEEK_SET);
				fwrite(block, sizeof(unsigned char), sizeof(block), image);
				size = size - modsize;
				total = total + modsize;
			}
			fat[index * 2] = 0xFF;
			fat[index * 2 + 1] = 0xFC;
			return total;
		}
		//write for overwriting data
		else {
			while(size > 0) {
				//loads the contents of the first block
				fseek(image, size * index, SEEK_SET);
				fread(block, 1, sizeof(block), image);		
			
				//moves till the right offset 
				if( offset > 512) {
					offset = offset - 512;
				}
				else {
					if(size > 512) {
						modsize = 512;
					}
					else { 
						modsize = size;
					}
					//writes the need bytes taking into account of remaining offset
					if (offset == 0) {
						memcpy(block, buf + total, modsize);
					}
					else {
						memcpy(block + offset, buf + total, (modsize - offset));
						offset = 0;
					}
					size = size - modsize;
					total = total + modsize;
				}
				fseek(image, size * index, SEEK_SET);
				fwrite(block, sizeof(unsigned char), sizeof(block), image);

				//check the fat to get next block
				memcpy(blockIndex, fat + (index * 2), 2);
				index = blockIndex[1]*0x100 + blockIndex[0];
				//stops if end of chain marker		
				if (index >= 65530) {
					return total;
				}			
			}
		}
		return total;	
	}
	return -ENOENT;	
}

static int vmufs_unlink(const char* path){
	int counter, index, i;
	unsigned char blockIndex[2];
	setFat();

	if ((counter = find(path)) >= 0) {
		//set the FAT entries to being empty
		memcpy(blockIndex, block + counter + 2, 2);
		index = blockIndex[1]*0x100 + blockIndex[0];
		if (index < 0) {
			return 0;
		}
		while (index < 65530) {
			memcpy(blockIndex, fat + (index * 2), 2);
			fat[index * 2] = 0xFC;
			fat[index * 2 + 1] = 0xFF;
			index = blockIndex[1]*0x100 + blockIndex[0];
		}
		fseek(image, (size * 254), SEEK_SET);
		fwrite(fat, sizeof(unsigned char), sizeof(fat), image);
		//clears the file information from the directory 		
		for (i = 0; i < 32; i++) {
			block[counter + i] = 0x00;
		}
		fseek(image, (size * curBlock), SEEK_SET);
		fwrite(block, sizeof(unsigned char), sizeof(block), image);
		return 0;
	}
	return -ENOENT;
}

static int vmufs_rename(const char* from, const char* to) {
	int counter;
	char* slash;

	if ((counter = find(from)) >= 0) {	
		counter = counter + 4;
		slash = strrchr(to, '/');
		memcpy(block + counter, slash + 1, 12 * sizeof(unsigned char));
		fseek(image, (size * curBlock), SEEK_SET);
		fwrite(block, sizeof(unsigned char), sizeof(block), image);
		return 0;
	}
	return -ENOENT;
}

static int vmufs_create(const char* path, mode_t mode, struct fuse_file_info *fi) {
	//if file doesn't exist, then fill in it's directory block information and then find the first empty block in userspace data and fill in the corresponding fat.	
	int numBlock = 253, counter,i;
	char timestamp[17]; 
	char *slash;
	time_t t;
	struct tm *tmp;

	if (find(path) < 0) {
		while (numBlock >= 240) {
			counter = 4;		
			while (counter < 512) {	
				if (block[counter] == 0x00) {
					block[counter - 4] = mode;
					block[counter - 3] = 0x00;
					block[counter - 2] = 0xFF;
					block[counter - 1] = 0xFF;
					slash = strrchr(path, '/');
					memcpy(block+counter, slash + 1, 12 * sizeof(unsigned char));
					//add time stuff here
					time(&t);
					tmp = localtime(&t);
				 	if (strftime(timestamp, sizeof(timestamp), "%G%m%d%H%M%S0%u", tmp) != 0) {
						for(i = 0; i < sizeof(timestamp); i = i + 2) {
							block[counter + 12 + i] = formatBCD(timestamp, i, i + 1);	
						}	
					}
					block[counter + 20] = 0x00;
					block[counter + 22] = 0x00;
					for(i = 0; i < 4; i++) {
						block[counter + 24 + i] = 0x00;
					}
					fseek(image, (size * curBlock), SEEK_SET);
					fwrite(block, sizeof(unsigned char), sizeof(block), image);
					return 0;
				}
				else {				
					counter = counter + 32;
				}
			}
			numBlock--;	
		}
	}		
	return -errno;
}

static struct fuse_operations vmufs_oper = {
	.getattr	= vmufs_getattr,
	.readdir	= vmufs_readdir,
	.open		= vmufs_open,
	.read		= vmufs_read,
	.write		= vmufs_write,
	.unlink 	= vmufs_unlink,
	.rename 	= vmufs_rename,
	.create		= vmufs_create,
};

int main(int argc, char** argv) {
	umask(022);
	filebuf = argv[1];
	image = fopen(filebuf,"rb+");

	//parsing the arguements so there is only the mount left
	int exelen, mntlen;
	char* array[3];		
	exelen = strlen(argv[0]);
	mntlen = strlen(argv[2]);
	
	char exe[exelen], mnt[mntlen];
	strncpy(exe,argv[0],exelen);
	strncpy(mnt,argv[2],mntlen);
	array[0] = exe;
	array[1] = mnt;
	array[2] = '\0';
	argc--;

	return fuse_main(argc, array, &vmufs_oper, NULL);
}
