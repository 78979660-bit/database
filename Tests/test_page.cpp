#include <iostream>
#include <cstring>
#include "Page.h"

int main()
{
    // 1. 创建一个 Page 对象
    Page myPage;

    // 2. 检查初始状态
    std::cout << "Initial Page ID: " << myPage.GetPageId() << std::endl;
    std::cout << "Initial Pin Count: " << myPage.GetPinCount() << std::endl;

    // 3. 向 Page 中写入数据
    // 我们获取 Page 的数据指针，就像操作普通内存一样
    char *data = myPage.GetData();
    const char *message = "Hello, Database World!";

    // 将字符串复制到 Page 的数据区
    std::memcpy(data, message, std::strlen(message) + 1);

    // 4. 读取数据
    std::cout << "Data in Page: " << data << std::endl;

    // 5. 模拟一些元数据操作 (通常由 BufferPoolManager 完成)
    // 假设我们修改了页面，标记为脏页
    // 注意：Page 类本身没有 SetDirty 方法，因为这通常由管理层控制，
    // 但为了演示，我们可以通过友元类或者直接修改（如果我们在测试中放宽权限）
    // 在实际开发中，你会有一个 BufferPoolManager 来管理这些状态。

    std::cout << "Page Size: " << PAGE_SIZE << " bytes" << std::endl;

    return 0;
}
