// 获取页面上的 DOM 元素
const usernameInput = document.getElementById('username');
const passwordInput = document.getElementById('password');
const messageDiv = document.getElementById('message');

// 封装发送请求的函数
async function sendRequest(path) {
    const username = usernameInput.value.trim();
    const password = passwordInput.value;
    // 检查非空
    if (!username || !password) {
        messageDiv.style.color = 'red';
        messageDiv.innerText = '用户名和密码不能为空！';
        return;
    }
    // 清空提示
    messageDiv.innerText = '';

    try {  // 用 Fetch API 发送 POST 请求
        const response = await fetch(path, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ username: username, password: password }) // 把对象转成 JSON 字符串
        });
        // 解析服务端返回的 JSON 数据
        const data = await response.json();
        // 根据服务端返回的数据更新界面
        if (data.code === 200) {
            messageDiv.style.color = 'green';
            messageDiv.innerText = data.msg;
            // 登陆成功把 token 存下来
            if (path === '/api/login' && data.token) {
                localStorage.setItem('token', data.token);
                messageDiv.innerText += ',即将跳转...'; // 跳转聊天界面
            }
        } else {
            messageDiv.style.color = 'red';
            messageDiv.innerText = data.msg;
        }
    } catch (error) {
        messageDiv.style.color = 'red';
        messageDiv.innerText = '网络错误，请求失败！';
        console.error("Fecth Error:", error);
    }
}

// 绑定按钮点击事件
document.getElementById('btn-register').addEventListener('click', () => {
    sendRequest('/api/register');
});
document.getElementById('btn-login').addEventListener('click', () => {
    sendRequest('/api/login');
});