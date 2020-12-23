#include <vector>
#include "bigmap.h"
#include "indexer.h"

typedef std::vector<bits24> bits24_vec;

// compact at most 3 20-bit integers into one 64-bit integer
// bit 62 and 61 indicate the count (1, 2 or 3)
uint64_t compact_index_list(uint32_t* index_ptr, int index_count) {
	uint64_t res = 0;
	for(int i=0; i<index_count && i<3; i++) {
		res |= uint64_t(index_ptr[i])<<(i*20);
	}
	res |= uint64_t(index_count) << 61;
	return res;
}

struct bits24_list {
	bits24 arr[3];
	bits24* data;
	int size;
	// expand one 64-bit integer into 1, 2 or 3 20-bit integers.
	static bits24_list from_uint64(uint64_t u) {
		bits24_list l;
		l.data = l.arr;
		l.size = u >> 61;
		assert(l.size <= 3);
		for(int i=0; i<l.size; i++) {
			l.arr[i] = bits24::from_uint64(u>>(i*20));
		}
		return l;
	}
};

//| Name                 | Key                         | Value                   |
//| -------------------- | --------------------------- | ----------------------- |
//| Block Content        | Height1 + 3 + Offset5       | Pointer to TxIndex3 Vec |
//| BlockHash Index      | ShortHashID6                | Height4                 |
//| Transaction Content  | Height4 + TxIndex3          | Offset5                 |
//| TransactionHash Index| ShortHashID6                | Offset5                 |
//| Address to TxKey     | ShortHashID6 + BlockHeight4 | Magic Uint64            |
//| Topic to TxKey       | ShortHashID6 + BlockHeight4 | Magic Uint64            |

// We carefully choose the data types to make sure there are no padding bytes in 
// the leaf nodes of btree_map (which are also called as target nodes)
// positions are actually the value for block heights, but in blk_htpos2ptr, we
// take them as part of keys, just to avoid padding bytes.
typedef bigmap<(1<<8),  uint64_t, bits24_vec*> blk_htpos2ptr;
typedef bigmap<(1<<16), uint32_t, uint32_t>    blk_hash2ht;
typedef bigmap<(1<<16), bits40,   bits40>      tx_id2pos;
typedef bigmap<(1<<16), bits32,   bits40>      tx_hash2pos;
typedef bigmap<(1<<16), uint64_t, uint64_t>    log_map;

class indexer {
	blk_htpos2ptr blk_htpos2ptr_map;
	blk_hash2ht   blk_hash2ht_map;
	tx_id2pos     tx_id2pos_map;
	tx_hash2pos   tx_hash2pos_map;
	log_map       addr_map;
	log_map       topic_map;

	typename blk_htpos2ptr::basic_map::iterator get_iter_at_height(uint32_t height, bool* ok);
public:
	indexer() {};
	indexer(const indexer& other) = delete;
	indexer& operator=(const indexer& other) = delete;
	indexer(indexer&& other) = delete;
	indexer& operator=(indexer&& other) = delete;

	bool add_block(uint32_t height, uint64_t hash48, int64_t offset40);
	void erase_block(uint32_t height, uint64_t hash48);
	int64_t offset_by_block_height(uint32_t height);
	bits24_vec* get_vec_at_height(uint32_t height, bool create_if_null);
	int64_t offset_by_block_hash(uint64_t hash48);
	bool add_tx(uint64_t id56, uint64_t hash48, int64_t offset40);
	void erase_tx(uint64_t id56, uint64_t hash48);
	int64_t offset_by_tx_id(uint64_t id56);
	int64_t offset_by_tx_hash(uint64_t hash48);

	void add_to_log_map(log_map& m, uint64_t hash48, uint32_t height, uint32_t* index_ptr, int index_count);

	void erase_in_log_map(log_map& m, uint64_t hash48, uint32_t height) {
		m.erase(hash48>>32, (hash48<<32)|uint64_t(height));
	}

	void add_addr2log(uint64_t hash48, uint32_t height, uint32_t* index_ptr, int index_count) {
		add_to_log_map(addr_map, hash48, height, index_ptr, index_count);
	}
	void erase_addr2log(uint64_t hash48, uint32_t height) {
		erase_in_log_map(addr_map, hash48, height);
	}
	void add_topic2log(uint64_t hash48, uint32_t height, uint32_t* index_ptr, int index_count) {
		add_to_log_map(topic_map, hash48, height, index_ptr, index_count);
	}
	void erase_topic2log(uint64_t hash48, uint32_t height) {
		erase_in_log_map(topic_map, hash48, height);
	}

	// iterator over tx's id56
	class tx_iterator {
		indexer*    _parent; 
		bits24_list _curr_list; //current block's bits24_list
		int         _curr_list_idx; //pointing to an element in _curr_list.data
		typename log_map::iterator _iter;
	public:
		friend class indexer;
		bool valid() {
			return _iter.valid() && _curr_list_idx < _curr_list.size;
		}
		uint64_t value() {//returns id56: 4 bytes height and 3 bytes offset
			if(!valid()) return uint64_t(-1);
			auto height = uint64_t(uint32_t(_iter.key())); //discard the high 32 bits of Key
			return (height<<24)|_curr_list.data[_curr_list_idx].to_uint64();
		}
		void next() {
			if(!valid()) return;
			if(_curr_list_idx < _curr_list.size) { //within same height
				_curr_list_idx++;
				return;
			}
			_iter.next(); //to the next height
			if(!_iter.valid()) return;
			load_list();
		}
		void load_list() {//fill data to _curr_list
			_curr_list_idx = 0;
			auto magic_u64 = _iter.value();
			auto height = uint32_t(_iter.key());
			auto tag = magic_u64>>61;
			magic_u64 = (magic_u64<<3)>>3; //clear the tag
			if(tag == 7) { // more than 3 members. find them in block's bits24_vec
				auto vec = _parent->get_vec_at_height(height, false);
				assert(vec != nullptr);
				_curr_list.size = vec->at(magic_u64).to_uint64();
				_curr_list.data = vec->data() + magic_u64 + 1;
			} else { // no more than 3 members. extract them out from magic_u64
				assert(tag != 0 && tag <= 3);
				_curr_list = bits24_list::from_uint64(magic_u64);
			}
			assert(_curr_list.size != 0);
		}
	};

private:
	tx_iterator _iterator_at_log_map(log_map& m, uint64_t hash48, uint32_t start_height, uint32_t end_height) {
		tx_iterator it;
		it._parent = this;
		it._iter = m.get_iterator(hash48>>32, (hash48<<32)|uint64_t(start_height),
		                          hash48>>32, (hash48<<32)|uint64_t(end_height));
		it.load_list();
		return it;
	}
public:
	tx_iterator addr_iterator(uint64_t hash48, uint32_t start_height, uint32_t end_height) {
		return _iterator_at_log_map(this->addr_map, hash48, start_height, end_height);
	}
	tx_iterator topic_iterator(uint64_t hash48, uint32_t start_height, uint32_t end_height) {
		return _iterator_at_log_map(this->topic_map, hash48, start_height, end_height);
	}
	i64_list query_tx_offsets(tx_offsets_query q);
};

// add a new block's information, return whether this 'hash48' is available to use
bool indexer::add_block(uint32_t height, uint64_t hash48, int64_t offset40) {
	auto vec = get_vec_at_height(height-1, false);
	//shrink the previous block's bits24_vec to save memory
	if(vec != nullptr) vec->shrink_to_fit();
	//check if hash48 has been used before
	bool ok;
	auto it = blk_hash2ht_map.seek(hash48>>32, uint32_t(hash48), &ok);
	if(ok && it.key() == uint32_t(hash48)) return false; //hash48 conflict
	// concat the low 3 bytes of height and 5 bytes of offset40 into ht3off5
	uint64_t ht3off5 = (uint64_t(height)<<40) | ((uint64_t(offset40)<<24)>>24);
	blk_htpos2ptr_map.insert(height>>24, ht3off5, nullptr);
	blk_hash2ht_map.insert(hash48>>32, uint32_t(hash48), height);
	return true;
}

// given a block's height, return the corresponding iterator
// *ok indicates whether this iterator is valid
typename blk_htpos2ptr::basic_map::iterator indexer::get_iter_at_height(uint32_t height, bool* ok) {
	uint64_t ht3off5 = (uint64_t(height)<<40);
	auto it = blk_htpos2ptr_map.seek(height>>24, ht3off5, ok);
	*ok = (*ok) && (ht3off5>>40) == (it->first>>40); //whether the 3 bytes of height matches
	return it;
}

// erase an old block's information
void indexer::erase_block(uint32_t height, uint64_t hash48) {
	bool ok;
	auto it = get_iter_at_height(height, &ok);
	if(ok) {
		delete it->second; // free the bits24_vec
		blk_htpos2ptr_map.erase(height>>24, it->first);
	}
	blk_hash2ht_map.erase(hash48>>32, uint32_t(hash48));
}

// given a block's height, return its offset
int64_t indexer::offset_by_block_height(uint32_t height) {
	bool ok;
	auto it = get_iter_at_height(height, &ok);
	if(!ok) {
		return -1;
	}
	return (it->first<<24)>>24; //offset40 is embedded in key's low 40 bits
}

// get the bits24_vec for height
bits24_vec* indexer::get_vec_at_height(uint32_t height, bool create_if_null) {
	bool ok;
	auto it = get_iter_at_height(height, &ok);
	if(!ok) return nullptr; //no such height
	if(it->second == nullptr && create_if_null) {
		auto v = new bits24_vec;
		blk_htpos2ptr_map.insert(height>>24, it->first, v);
		return v;
	}
	return it->second;
}

// given a block's hash48, return its offset
int64_t indexer::offset_by_block_hash(uint64_t hash48) {
	bool ok;
	uint32_t height = blk_hash2ht_map.get(hash48>>32, uint32_t(hash48), &ok);
	if(!ok) return -1;
	return offset_by_block_height(height);
}

// A transaction's id has 56 bits: 32 bits height + 24 bits in-block index
// add a new transaction's information, return whether hash48 is available to use
bool indexer::add_tx(uint64_t id56, uint64_t hash48, int64_t offset40) {
	bool ok;
	tx_hash2pos_map.get(hash48>>32, bits32::from_uint64(hash48), &ok);
	if(ok) return false; //hash48 conflict
	auto off40 = bits40::from_int64(offset40);
	tx_id2pos_map.insert(id56>>40, bits40::from_uint64(id56), off40);
	tx_hash2pos_map.insert(hash48>>32, bits32::from_uint64(hash48), off40);
	return true;
}

// erase a old transaction's information
void indexer::erase_tx(uint64_t id56, uint64_t hash48) {
	tx_id2pos_map.erase(id56>>40, bits40::from_uint64(id56));
	tx_hash2pos_map.erase(hash48>>32, bits32::from_uint64(hash48));
}

// given a transaction's 56-bit id, return its offset
int64_t indexer::offset_by_tx_id(uint64_t id56) {
	bool ok;
	auto off = tx_id2pos_map.get(id56>>40, bits40::from_uint64(id56), &ok);
	if(!ok) return -1;
	return off.to_int64();
}

// given a transaction's hash48, return its offset
int64_t indexer::offset_by_tx_hash(uint64_t hash48) {
	bool ok;
	auto off = tx_hash2pos_map.get(hash48>>32, bits32::from_uint64(hash48), &ok);
	if(!ok) return -1;
	return off.to_int64();
}

// given a log_map m, add new information into it
void indexer::add_to_log_map(log_map& m, uint64_t hash48, uint32_t height, uint32_t* index_ptr, int index_count) {
	uint64_t v;
	assert(index_count>=0);
	if(index_count <= 3) { // store the indexes as an in-place integer
		v = compact_index_list(index_ptr, index_count);
	} else { // store the indexes in a bits24_vec shared by all the logs in a block
		auto vec = get_vec_at_height(height, true);
		assert(vec != nullptr);
		v = vec->size(); //pointing to the start of the new members
		v |= uint64_t(7)<<61; // the highest three bits are all set to 1
		vec->push_back(bits24::from_uint32(index_count)); //add a member for size
		for(int i=0; i<index_count; i++) {  // add members for indexes
			vec->push_back(bits24::from_uint32(index_ptr[i]));
		}
	}
	m.insert(hash48>>32, (hash48<<32)|uint64_t(height), v);
}

// the iterators in vector are all valid
bool iters_all_valid(std::vector<indexer::tx_iterator>& iters) {
	assert(iters.size() != 0);
	for(int i=0; i<iters.size(); i++) {
		if(!iters[i].valid()) return false;
	}
	return true;
}

// the iterators in vector are all pointing to same value
bool iters_value_all_equal(std::vector<indexer::tx_iterator>& iters) {
	assert(iters.size() != 0);
	for(int i=1; i<iters.size(); i++) {
		if(iters[i].value() != iters[0].value()) return false;
	}
	return true;
}

// given query condition 'q', query a list of offsets for transactions
i64_list indexer::query_tx_offsets(tx_offsets_query q) {
	auto i64_vec = new std::vector<int64_t>;
	std::vector<indexer::tx_iterator> iters;
	if(q.addr_hash>>48 == 0) {// only consider valid hash
		iters.push_back(addr_iterator(q.addr_hash, q.start_height, q.end_height));
	}
	for(int i=0; i<q.topic_count; i++) {
		iters.push_back(topic_iterator(q.topic_hash[i], q.start_height, q.end_height));
	}
	if(iters.size() == 0) {
		return i64_list{.vec_ptr=nullptr, .data=nullptr, .size=0};
	}
	for(bool all_valid=iters_all_valid(iters); all_valid ; iters[0].next()) {
		for(int i=1; i<iters.size(); i++) {
			while(iters[i].valid() && iters[i].value() <= iters[0].value()) {
				iters[i].next(); //all the others mutch catch up with iters[0]
			}
		}
		all_valid = iters_all_valid(iters);
		if(all_valid && iters_value_all_equal(iters)) { // found a matching tx
			i64_vec->push_back(offset_by_tx_id(iters[0].value()));
		}
	}
	return i64_list{.vec_ptr=i64_vec, .data=i64_vec->data(), .size=i64_vec->size()};
}

// =============================================================================

size_t indexer_create() {
	return (size_t)(new indexer);
}

void indexer_destroy(size_t ptr) {
	delete (indexer*)ptr;
}

bool indexer_add_block(size_t ptr, uint32_t height, uint64_t hash48, int64_t offset40) {
	return ((indexer*)ptr)->add_block(height, hash48, offset40);
}

void indexer_erase_block(size_t ptr, uint32_t height, uint64_t hash48) {
	((indexer*)ptr)->erase_block(height, hash48);
}

int64_t indexer_offset_by_block_height(size_t ptr, uint32_t height) {
	return ((indexer*)ptr)->offset_by_block_height(height);
}

int64_t indexer_offset_by_block_hash(size_t ptr, uint64_t hash48) {
	return ((indexer*)ptr)->offset_by_block_hash(hash48);
}

bool indexer_add_tx(size_t ptr, uint64_t id56, uint64_t hash48, int64_t offset40) {
	return ((indexer*)ptr)->add_tx(id56, hash48, offset40);
}

void indexer_erase_tx(size_t ptr, uint64_t id56, uint64_t hash48) {
	((indexer*)ptr)->erase_tx(id56, hash48);
}

int64_t indexer_offset_by_tx_id(size_t ptr, uint64_t id56) {
	return ((indexer*)ptr)->offset_by_tx_id(id56);
}

int64_t indexer_offset_by_tx_hash(size_t ptr, uint64_t hash48) {
	return ((indexer*)ptr)->offset_by_tx_hash(hash48);
}

void indexer_add_addr2log(size_t ptr, uint64_t hash48, uint32_t height, uint32_t* index_ptr, int index_count) {
	((indexer*)ptr)->add_addr2log(hash48, height, index_ptr, index_count);
}

void indexer_erase_addr2log(size_t ptr, uint64_t hash48, uint32_t height) {
	((indexer*)ptr)->erase_addr2log(hash48, height);
}

void indexer_add_topic2log(size_t ptr, uint64_t hash48, uint32_t height, uint32_t* index_ptr, int index_count) {
	((indexer*)ptr)->add_topic2log(hash48, height, index_ptr, index_count);
}

void indexer_erase_topic2log(size_t ptr, uint64_t hash48, uint32_t height) {
	((indexer*)ptr)->erase_topic2log(hash48, height);
}

i64_list indexer_query_tx_offsets(size_t ptr, tx_offsets_query q) {
	return ((indexer*)ptr)->query_tx_offsets(q);
}

void i64_list_destroy(i64_list l) {
	delete (std::vector<int64_t>*)l.vec_ptr;
}
