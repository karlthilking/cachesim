#include <vector>
#include <cstdint>
#include <algorithm>

struct node {
    node    *next;
    int     val;
    char    pad[64 - sizeof(node *) - sizeof(int)];

    node() : next(nullptr), val(0) {}
    node(int x) : next(nullptr), val(x) {}
};

struct aligned_char {
    alignas(64) char __c;
    
    aligned_char() = default;
    aligned_char(char c) : __c(c) {}

    void operator=(char c) { __c = c; }
};

int main(void)
{
    std::vector<aligned_char> v0(0x400, '\0');
    for (auto i = 0u; i < 0x400; i += 1) {
        char c = v0[i].__c;
        v0[i].__c = c + 1;
    }

    std::vector<size_t> v1(0x400, 100u);
    for (auto i = 0u; i < 0x400; i += (64 / sizeof(size_t))) {
        v1[i] *= 2;
        v1[i] /= 2;
    }

    aligned_char *chars = new aligned_char[1 << 10];
    for (auto *c = chars; c < chars + 1024u; c++)
        c->__c = '\0';
    delete[] chars;
    
    for (auto iter = 0u; iter < 1024u; iter++) {
        node *head = new node;
        node *n = nullptr;
        int i;
        for (n = head, i = 0; i < 1024; i++, n = n->next)
            n->next = new node(i);
        
        n = head;
        while (n) {
            node *temp = n->next;
            delete n;
            n = temp;
        }

        for (auto j = 0u; j < 128u; j++) {
            aligned_char *c = new aligned_char('A');
            *c = '0' + 'A';
            delete c;
        }
    }

    return 0;
}
