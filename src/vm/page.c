/* TODO:

implement using hash table
key: VA
value: struct sup_page_table_entry

enum data_types
  {
    STACK = 0;
    EXEC = 1;
    FILE = 2;
  };

struct sup_page_entry {
  struct frame_entry *frame_entry;
  enum data_types;
  struct file *file;
  int offset;
  bool in_swap    // this means in swap disk and not on hard disk
  int file_index  // this gives the index into swap or disk
  bool is_code    // this means in code segment and not data segment of exec
};
