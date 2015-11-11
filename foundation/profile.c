/* profile.c  -  Foundation library  -  Public Domain  -  2013 Mattias Jansson / Rampant Pixels
 *
 * This library provides a cross-platform foundation library in C11 providing basic support
 * data types and functions to write applications and games in a platform-independent fashion.
 * The latest source code is always available at
 *
 * https://github.com/rampantpixels/foundation_lib
 *
 * This library is put in the public domain; you can redistribute it and/or modify it without
 * any restrictions.
 */

#include <foundation/foundation.h>
#include <foundation/internal.h>

#if BUILD_ENABLE_PROFILE

#define PROFILE_ENABLE_SANITY_CHECKS 0

typedef struct _profile_block_data   profile_block_data_t;
typedef struct _profile_block        profile_block_t;
typedef struct _profile_root         profile_root_t;

#define MAX_MESSAGE_LENGTH 25

#pragma pack(push)
#pragma pack(1)

struct _profile_block_data {
	int32_t               id;
	int32_t               parentid;
	uint32_t              processor;
	uint32_t              thread;
	tick_t                start;
	tick_t                end;
	char                  name[ MAX_MESSAGE_LENGTH + 1 ];
}; //sizeof( profile_block_data ) == 58
FOUNDATION_STATIC_ASSERT(sizeof(profile_block_data_t) == 58, "profile_block_data_t size");

#pragma pack(pop)

struct _profile_block {
	profile_block_data_t  data;
	uint16_t              previous;
	uint16_t              sibling;
	uint16_t              child;
}; //sizeof( profile_block ) == 64
FOUNDATION_STATIC_ASSERT(sizeof(profile_block_t) == 64, "profile_block_t size");

//Continue values generated by +1 in block split
#define PROFILE_ID_ENDOFSTREAM      0
#define PROFILE_ID_SYSTEMINFO       1
#define PROFILE_ID_LOGMESSAGE       2
//#define PROFILE_ID_LOGCONTINUE      3
#define PROFILE_ID_ENDFRAME         4
#define PROFILE_ID_TRYLOCK          5
//#define PROFILE_ID_TRYLOCKCONTINUE  6
#define PROFILE_ID_LOCK             7
//#define PROFILE_ID_LOCKCONTINUE     8
#define PROFILE_ID_UNLOCK           9
//#define PROFILE_ID_UNLOCKCONTINUE   10
#define PROFILE_ID_WAIT             11
#define PROFILE_ID_SIGNAL           12

#define GET_BLOCK( index )          ( _profile_blocks + (index) )
#define BLOCK_INDEX( block )        (uint16_t)((uintptr_t)( (block) - _profile_blocks ))

static string_const_t   _profile_identifier;
static atomic32_t       _profile_counter;
static atomic32_t       _profile_loopid;
static atomic32_t       _profile_free;
static atomic32_t       _profile_root;
static profile_block_t* _profile_blocks;
static tick_t           _profile_ground_time;
static int              _profile_enable;
static profile_write_fn _profile_write;
static uint64_t         _profile_num_blocks;
static unsigned int     _profile_wait = 100;
static thread_t         _profile_io_thread;
static semaphore_t      _profile_io_exit;

FOUNDATION_DECLARE_THREAD_LOCAL(int32_t, profile_block, 0)

static profile_block_t*
_profile_allocate_block(void) {
	//Grab block from free list, avoiding ABA issues by
	//using high 16 bit as a loop counter
	profile_block_t* block;
	int32_t free_block_tag, free_block, next_block_tag;
	do {
		free_block_tag = atomic_load32(&_profile_free);
		free_block = free_block_tag & 0xffff;

		next_block_tag = GET_BLOCK(free_block)->child;
		next_block_tag |= (atomic_incr32(&_profile_loopid) & 0xffff) << 16;
	}
	while (free_block && !atomic_cas32(&_profile_free, next_block_tag, free_block_tag));

	if (!free_block) {
		static atomic32_t has_warned = {0};
		if (atomic_cas32(&has_warned, 1, 0)) {
			if (_profile_num_blocks < 65535)
				log_error(0, ERROR_OUT_OF_MEMORY,
				          STRING_CONST("Profile blocks exhausted, increase profile memory block size"));
			else
				log_error(0, ERROR_OUT_OF_MEMORY,
				          STRING_CONST("Profile blocks exhausted, decrease profile output wait time"));
		}
		return 0;
	}

	block = GET_BLOCK(free_block);
	memset(block, 0, sizeof(profile_block_t));
	return block;
}

static void
_profile_free_block(int32_t block, int32_t leaf) {
	int32_t last_tag, block_tag;
	do {
		block_tag = block | ((atomic_incr32(&_profile_loopid) & 0xffff) << 16);
		last_tag = atomic_load32(&_profile_free);
		GET_BLOCK(leaf)->child = last_tag & 0xffff;
	}
	while (!atomic_cas32(&_profile_free, block_tag, last_tag));
}

static void
_profile_put_root_block(int32_t block) {
	uint16_t sibling;
	profile_block_t* self = GET_BLOCK(block);

#if PROFILE_ENABLE_SANITY_CHECKS
	FOUNDATION_ASSERT(self->sibling == 0);
#endif
	while (!atomic_cas32(&_profile_root, block, 0)) {
		do {
			sibling = (uint16_t)atomic_load32(&_profile_root);
		}
		while (sibling && !atomic_cas32(&_profile_root, 0, (int32_t)sibling));

		if (sibling) {
			if (self->sibling) {
				uint16_t leaf = self->sibling;
				while (GET_BLOCK(leaf)->sibling)
					leaf = GET_BLOCK(leaf)->sibling;
				GET_BLOCK(sibling)->previous = leaf;
				GET_BLOCK(leaf)->sibling = sibling;
			}
			else {
				self->sibling = sibling;
			}
		}
	}
}

static void
_profile_put_simple_block(int32_t block) {
	//Add to current block, or if no current add to array
	int32_t parent_block = get_thread_profile_block();
	if (parent_block) {
		profile_block_t* self = GET_BLOCK(block);
		profile_block_t* parent = GET_BLOCK(parent_block);
		int32_t next_block = parent->child;
		self->previous = (uint16_t)parent_block;
		self->sibling = (uint16_t)next_block;
		if (next_block)
			GET_BLOCK(next_block)->previous = (uint16_t)block;
		parent->child = (uint16_t)block;
	}
	else {
		_profile_put_root_block(block);
	}
}

static void
_profile_put_message_block(int32_t id, const char* message, size_t length) {
	profile_block_t* subblock = 0;

	//Allocate new master block
	profile_block_t* block = _profile_allocate_block();
	if (!block)
		return;
	block->data.id = id;
	block->data.processor = thread_hardware();
	block->data.thread = (uint32_t)thread_id();
	block->data.start  = time_current() - _profile_ground_time;
	block->data.end = atomic_add32(&_profile_counter, 1);
	string_copy(block->data.name, sizeof(block->data.name), message, length);

	length = (length > MAX_MESSAGE_LENGTH ? length - MAX_MESSAGE_LENGTH : 0);
	message += MAX_MESSAGE_LENGTH;
	subblock = block;

	while (length > 0) {
		//add subblock
		profile_block_t* cblock = _profile_allocate_block();
		uint16_t cblock_index;
		if (!cblock)
			return;
		cblock_index = BLOCK_INDEX(cblock);
		cblock->data.id = id + 1;
		cblock->data.parentid = (int32_t)subblock->data.end;
		cblock->data.processor = block->data.processor;
		cblock->data.thread = block->data.thread;
		cblock->data.start  = block->data.start;
		cblock->data.end    = atomic_add32(&_profile_counter, 1);
		string_copy(cblock->data.name, sizeof(cblock->data.name), message, length);

		cblock->sibling = subblock->child;
		if (cblock->sibling)
			GET_BLOCK(cblock->sibling)->previous = cblock_index;
		subblock->child = cblock_index;
		cblock->previous = BLOCK_INDEX(subblock);
		subblock = cblock;

		length = (length > MAX_MESSAGE_LENGTH ? length - MAX_MESSAGE_LENGTH : 0);
		message += MAX_MESSAGE_LENGTH;
	}

	_profile_put_simple_block(BLOCK_INDEX(block));
}

//Pass each block once, writing it to stream and adjusting child/sibling pointers to form
//a single-linked list through child pointer. Potential drawback of this is that block access
//order will degenerate over time and result in random access over the whole profile memory
//area in the end
static profile_block_t*
_profile_process_block(profile_block_t* block) {
	profile_block_t* leaf = block;

	if (_profile_write)
		_profile_write(block, sizeof(profile_block_t));

	if (block->child) {
		leaf = _profile_process_block(GET_BLOCK(block->child));
		if (block->sibling) {
			profile_block_t* subleaf = _profile_process_block(GET_BLOCK(block->sibling));
			subleaf->child = block->child;
			block->child = block->sibling;
			block->sibling = 0;
		}
	}
	else if (block->sibling) {
		leaf = _profile_process_block(GET_BLOCK(block->sibling));
		block->child = block->sibling;
		block->sibling = 0;
	}
	return leaf;
}

static void
_profile_process_root_block(void) {
	int32_t block;

	do {
		block = atomic_load32(&_profile_root);
	}
	while (block && !atomic_cas32(&_profile_root, 0, block));

	while (block) {
		profile_block_t* leaf;
		profile_block_t* current = GET_BLOCK(block);
		int32_t next = current->sibling;

		current->sibling = 0;
		leaf = _profile_process_block(current);
		_profile_free_block(block, BLOCK_INDEX(leaf));

		block = next;
	}
}

static void*
_profile_io(void* arg) {
	unsigned int system_info_counter = 0;
	profile_block_t system_info;
	FOUNDATION_UNUSED(arg);
	memset(&system_info, 0, sizeof(profile_block_t));
	system_info.data.id = PROFILE_ID_SYSTEMINFO;
	system_info.data.start = time_ticks_per_second();
	string_copy(system_info.data.name, sizeof(system_info.data.name), "sysinfo", 7);

	while (!semaphore_try_wait(&_profile_io_exit, _profile_wait)) {

		if (!atomic_load32(&_profile_root))
			continue;

		profile_begin_block(STRING_CONST("profile_io"));

		if (atomic_load32(&_profile_root)) {
			profile_begin_block(STRING_CONST("process"));

			//This is thread safe in the sense that only completely closed and ended
			//blocks will be put as children to root block, so no additional blocks
			//will ever be added to child subtrees while we process it here
			_profile_process_root_block();

			profile_end_block();
		}

		if (system_info_counter++ > 10) {
			if (_profile_write)
				_profile_write(&system_info, sizeof(profile_block_t));
			system_info_counter = 0;
		}

		profile_end_block();
	}

	if (atomic_load32(&_profile_root))
		_profile_process_root_block();

	if (_profile_write) {
		profile_block_t terminate;
		memset(&terminate, 0, sizeof(profile_block_t));
		terminate.data.id = PROFILE_ID_ENDOFSTREAM;
		_profile_write(&terminate, sizeof(profile_block_t));
	}

	return 0;
}

void
profile_initialize(const char* identifier, size_t length, void* buffer, size_t size) {
	profile_block_t* root  = buffer;
	profile_block_t* block = root;
	uint32_t num_blocks = (uint32_t)(size / sizeof(profile_block_t));
	uint32_t i;

	if (num_blocks > 65535)
		num_blocks = 65535;

	for (i = 0; i < (num_blocks - 1); ++i, ++block) {
		block->child = (uint16_t)(i + 1);
		block->sibling = 0;
	}
	block->child = 0;
	block->sibling = 0;
	root->child = 0;

	atomic_store32(&_profile_root, 0);

	_profile_num_blocks = num_blocks;
	_profile_identifier = string_const(identifier, length);
	_profile_blocks = root;
	//TODO: Currently 0 is a no-block identifier, so we waste the first block
	atomic_store32(&_profile_free, 1);
	atomic_store32(&_profile_counter, 128);
	_profile_ground_time = time_current();
	set_thread_profile_block(0);

	semaphore_initialize(&_profile_io_exit, 0);
	thread_initialize(&_profile_io_thread, _profile_io, 0, STRING_CONST("profile_io"),
	                  THREAD_PRIORITY_BELOWNORMAL, 0);

	log_debugf(0, STRING_CONST("Initialize profiling system with %u blocks (%" PRIsize "KiB)"),
	           num_blocks, size / 1024);
}

void
profile_finalize(void) {
	profile_enable(0);

	semaphore_post(&_profile_io_exit);
	thread_finalize(&_profile_io_thread);
	semaphore_try_wait(&_profile_io_exit, 0);

	//Discard and free up blocks remaining in queue
	_profile_thread_finalize();
	if (atomic_load32(&_profile_root))
		_profile_process_root_block();

	//Sanity checks
	{
		uint64_t num_blocks = 0;
		uint32_t free_block = atomic_load32(&_profile_free) & 0xffff;

		if (atomic_load32(&_profile_root))
			log_error(0, ERROR_INTERNAL_FAILURE,
			          STRING_CONST("Profile module state inconsistent on finalize, "
			                       "at least one root block still allocated/active"));

		while (free_block) {
			profile_block_t* block = GET_BLOCK(free_block);
			if (block->sibling)
				log_errorf(0, ERROR_INTERNAL_FAILURE,
				           STRING_CONST("Profile module state inconsistent on finalize, "
				                        "block %d has sibling set"), free_block);
			++num_blocks;
			free_block = GET_BLOCK(free_block)->child;
		}
		if (_profile_num_blocks)
			++num_blocks; //Include the wasted block 0

		if (num_blocks != _profile_num_blocks) {
			//If profile output function (user) crashed, this will probably trigger
			//since at least one block will be lost in space
			log_errorf(0, ERROR_INTERNAL_FAILURE,
			           STRING_CONST("Profile module state inconsistent on finalize, lost blocks "
			                        "(found %" PRIu64 " of %" PRIu64 ")"),
			           num_blocks, _profile_num_blocks);
		}
	}

	atomic_store32(&_profile_root, 0);
	atomic_store32(&_profile_free, 0);

	_profile_num_blocks = 0;
	_profile_identifier = string_null();
}

void
profile_set_output(profile_write_fn writer) {
	_profile_write = writer;
}

void
profile_set_output_wait(unsigned int ms) {
	_profile_wait = (ms ? ms : 1U);
}

void
profile_enable(bool enable) {
	bool was_enabled = (_profile_enable > 0);
	bool is_enabled = enable;

	if (is_enabled && !was_enabled) {
		//Start output thread
		_profile_enable = 1;
		thread_start(&_profile_io_thread);
	}
	else if (!is_enabled && was_enabled) {
		//Stop output thread
		semaphore_post(&_profile_io_exit);
		thread_join(&_profile_io_thread);
		semaphore_try_wait(&_profile_io_exit, 0);
		_profile_enable = 0;
	}
}

void
profile_end_frame(uint64_t counter) {
	profile_block_t* block;
	if (!_profile_enable)
		return;

	//Allocate new master block
	block = _profile_allocate_block();
	if (!block)
		return;
	block->data.id = PROFILE_ID_ENDFRAME;
	block->data.processor = thread_hardware();
	block->data.thread = (uint32_t)thread_id();
	block->data.start  = time_current() - _profile_ground_time;
	block->data.end = (tick_t)counter;

	_profile_put_simple_block(BLOCK_INDEX(block));
}

void
profile_begin_block(const char* message, size_t length) {
	int32_t parent;
	if (!_profile_enable)
		return;

	parent = get_thread_profile_block();
	if (!parent) {
		//Allocate new master block
		profile_block_t* block = _profile_allocate_block();
		uint16_t blockindex;
		if (!block)
			return;
		blockindex = BLOCK_INDEX(block);
		block->data.id = atomic_add32(&_profile_counter, 1);
		string_copy(block->data.name, sizeof(block->data.name), message, length);
		block->data.processor = thread_hardware();
		block->data.thread = (uint32_t)thread_id();
		block->data.start  = time_current() - _profile_ground_time;
		set_thread_profile_block(blockindex);
	}
	else {
		//Allocate new child block
		profile_block_t* parentblock;
		profile_block_t* subblock = _profile_allocate_block();
		uint16_t subindex;
		if (!subblock)
			return;
		subindex = BLOCK_INDEX(subblock);
		parentblock = GET_BLOCK(parent);
		subblock->data.id = atomic_add32(&_profile_counter, 1);
		subblock->data.parentid = parentblock->data.id;
		string_copy(subblock->data.name, sizeof(subblock->data.name), message, length);
		subblock->data.processor = thread_hardware();
		subblock->data.thread = (uint32_t)thread_id();
		subblock->data.start  = time_current() - _profile_ground_time;
		subblock->previous = (uint16_t)parent;
		subblock->sibling = parentblock->child;
		if (parentblock->child)
			GET_BLOCK(parentblock->child)->previous = subindex;
		parentblock->child = subindex;
		set_thread_profile_block(subindex);
	}
}

void
profile_update_block(void) {
	char* message;
	unsigned int processor;
	int32_t block_index = get_thread_profile_block();
	profile_block_t* block;
	if (!_profile_enable || !block_index)
		return;

	block = GET_BLOCK(block_index);
	message = block->data.name;
	processor = thread_hardware();
	if (block->data.processor == processor)
		return;

	//Thread migrated to another core, split into new block
	profile_end_block();
	profile_begin_block(message, string_length(message));
}

void
profile_end_block(void) {
	int32_t block_index = get_thread_profile_block();
	profile_block_t* block;
	if (!_profile_enable || !block_index)
		return;

	block = GET_BLOCK(block_index);
	block->data.end = time_current() - _profile_ground_time;

	if (block->previous) {
		unsigned int processor;
		profile_block_t* current = block;
		profile_block_t* previous = GET_BLOCK(block->previous);
		profile_block_t* parent;
		int32_t current_index = block_index;
		uint16_t parent_index;
		while (previous->child != current_index) {
			current_index = current->previous; //Walk sibling list backwards
			current = GET_BLOCK(current_index);
			previous = GET_BLOCK(current->previous);
#if PROFILE_ENABLE_SANITY_CHECKS
			FOUNDATION_ASSERT(current_index != 0);
			FOUNDATION_ASSERT(current->previous != 0);
#endif
		}
		parent_index = current->previous; //Previous now points to parent
		parent = GET_BLOCK(parent_index);
#if PROFILE_ENABLE_SANITY_CHECKS
		FOUNDATION_ASSERT(parent_index != block_index);
#endif
		set_thread_profile_block(parent_index);

		processor = thread_hardware();
		if (parent->data.processor != processor) {
			const char* message = parent->data.name;
			//Thread migrated, split into new block
			profile_end_block();
			profile_begin_block(message, string_length(message));
		}
	}
	else {
		_profile_put_root_block(block_index);
		set_thread_profile_block(0);
	}
}

void
profile_log(const char* message, size_t length) {
	if (!_profile_enable)
		return;
	_profile_put_message_block(PROFILE_ID_LOGMESSAGE, message, length);
}

void
profile_trylock(const char* name, size_t length) {
	if (!_profile_enable)
		return;
	_profile_put_message_block(PROFILE_ID_TRYLOCK, name, length);
}

void
profile_lock(const char* name, size_t length) {
	if (!_profile_enable)
		return;
	_profile_put_message_block(PROFILE_ID_LOCK, name, length);
}

void
profile_unlock(const char* name, size_t length) {
	if (!_profile_enable)
		return;
	_profile_put_message_block(PROFILE_ID_UNLOCK, name, length);
}

void
profile_wait(const char* name, size_t length) {
	if (!_profile_enable)
		return;
	_profile_put_message_block(PROFILE_ID_WAIT, name, length);
}

void
profile_signal(const char* name, size_t length) {
	if (!_profile_enable)
		return;
	_profile_put_message_block(PROFILE_ID_SIGNAL, name, length);
}

#endif

void
_profile_thread_finalize(void) {
#if BUILD_ENABLE_PROFILE
	int32_t block_index, last_block = 0;
	while ((block_index = get_thread_profile_block())) {
		log_warnf(0, WARNING_SUSPICIOUS, STRING_CONST("Profile thread cleanup, free block %u"),
		          block_index);
		if (last_block == block_index) {
			log_warnf(0, WARNING_SUSPICIOUS, STRING_CONST("Unrecoverable error, self reference in block %u"),
			          block_index);
			break;
		}
		profile_end_block();
		last_block = block_index;
	}
#endif
}
