# C++ Systems Architecture Guide for AI Training

## Introduction

This document serves as a comprehensive guide for training an AI to become an expert architect in C++ systems and projects. The goal is to develop deep understanding of C++ language features, design principles, architectural patterns, and best practices that enable the creation of robust, maintainable, and high-performance software systems.

As an AI architect, you will need to master not only the syntax and semantics of C++, but also the higher-level thinking required to design scalable systems, make informed trade-offs, and guide teams toward quality implementations. This guide covers fundamental through advanced concepts, providing both theoretical knowledge and practical examples.

---

## C++ Fundamentals

### Core Language Features

C++ is a multi-paradigm programming language supporting procedural, object-oriented, and generic programming. Understanding its core features is essential for architecting systems.

#### Basic Types and Variables

```cpp
// Fundamental types
int count = 42;
double price = 19.99;
char initial = 'A';
bool isActive = true;

// Type inference (C++11+)
auto value = 42;        // int
auto pi = 3.14159;      // double
auto name = "John";     // const char*
```

#### References and Pointers

```cpp
int value = 10;
int& ref = value;       // Reference (alias)
int* ptr = &value;      // Pointer (address)

// Reference cannot be reseated, pointer can
ref = 20;               // Changes value
ptr = nullptr;          // ptr now points to nothing
```

#### Functions and Overloading

```cpp
// Function declaration
int add(int a, int b);

// Function overloading
double add(double a, double b);
std::string add(const std::string& a, const std::string& b);

// Default arguments
void log(const std::string& msg, int level = 0);

// Inline functions (compiler suggestion)
inline int square(int x) { return x * x; }
```

#### Templates (Generic Programming)

```cpp
// Function template
template<typename T>
T max(T a, T b) {
    return (a > b) ? a : b;
}

// Class template
template<typename T>
class Container {
    T* data_;
    size_t size_;
public:
    Container(size_t size) : data_(new T[size]), size_(size) {}
    ~Container() { delete[] data_; }
    T& operator[](size_t index) { return data_[index]; }
};
```

### Tooling Setup and Development Environments

#### Compilers

- **GCC (GNU Compiler Collection)**: Widely used, excellent C++ standards support
- **Clang**: Fast compilation, great error messages, part of LLVM
- **MSVC (Microsoft Visual C++)**: Windows-native, integrated with Visual Studio
- **Intel C++ Compiler**: Optimized for Intel architectures

```bash
# Compile with specific C++ standard
g++ -std=c++17 -O2 -Wall -Wextra main.cpp -o program
clang++ -std=c++20 -O3 -fsanitize=address main.cpp -o program
```

#### IDEs and Editors

- **Visual Studio**: Full-featured IDE for Windows development
- **CLion**: JetBrains IDE with intelligent code assistance
- **VS Code**: Lightweight with C++ extensions
- **Qt Creator**: Excellent for Qt-based projects
- **Vim/Emacs**: Powerful text editors with C++ support

#### Essential Tools

- **CMake**: Cross-platform build system generator
- **Make**: Traditional build automation
- **Ninja**: Fast small build system
- **Compiler Explorer (Godbolt)**: Online tool to view assembly output
- **clang-format**: Code formatting tool
- **clang-tidy**: Static analysis and linting
- **cppcheck**: Static analysis tool
- **Valgrind**: Memory leak detection and profiling
- **GDB/LLDB**: Debuggers

---

## Principles of Object-Oriented Design

### Classes and Objects

Classes are the foundation of object-oriented programming in C++. They encapsulate data and behavior.

```cpp
class BankAccount {
private:
    std::string accountNumber_;
    double balance_;
    
public:
    // Constructor
    BankAccount(const std::string& accNum, double initialBalance)
        : accountNumber_(accNum), balance_(initialBalance) {}
    
    // Member functions
    void deposit(double amount) {
        if (amount > 0) {
            balance_ += amount;
        }
    }
    
    bool withdraw(double amount) {
        if (amount > 0 && amount <= balance_) {
            balance_ -= amount;
            return true;
        }
        return false;
    }
    
    double getBalance() const { return balance_; }
};
```

### Encapsulation

Encapsulation hides internal implementation details and exposes only necessary interfaces.

**Benefits:**
- Protects data integrity
- Allows implementation changes without affecting clients
- Reduces coupling between components

```cpp
class TemperatureSensor {
private:
    double celsius_;
    
    // Private helper function
    double celsiusToFahrenheit(double c) const {
        return (c * 9.0 / 5.0) + 32.0;
    }
    
public:
    void setCelsius(double temp) { celsius_ = temp; }
    double getCelsius() const { return celsius_; }
    double getFahrenheit() const { return celsiusToFahrenheit(celsius_); }
};
```

### Inheritance

Inheritance enables code reuse and establishes "is-a" relationships.

```cpp
// Base class
class Shape {
protected:
    std::string color_;
    
public:
    Shape(const std::string& color) : color_(color) {}
    virtual ~Shape() = default;
    
    virtual double area() const = 0;  // Pure virtual (abstract)
    virtual double perimeter() const = 0;
    
    std::string getColor() const { return color_; }
};

// Derived class
class Circle : public Shape {
private:
    double radius_;
    
public:
    Circle(const std::string& color, double radius)
        : Shape(color), radius_(radius) {}
    
    double area() const override {
        return 3.14159 * radius_ * radius_;
    }
    
    double perimeter() const override {
        return 2 * 3.14159 * radius_;
    }
};

class Rectangle : public Shape {
private:
    double width_, height_;
    
public:
    Rectangle(const std::string& color, double w, double h)
        : Shape(color), width_(w), height_(h) {}
    
    double area() const override {
        return width_ * height_;
    }
    
    double perimeter() const override {
        return 2 * (width_ + height_);
    }
};
```

### Polymorphism

Polymorphism allows objects of different types to be treated uniformly through a common interface.

```cpp
void printShapeInfo(const Shape& shape) {
    std::cout << "Color: " << shape.getColor() << "\n";
    std::cout << "Area: " << shape.area() << "\n";
    std::cout << "Perimeter: " << shape.perimeter() << "\n";
}

int main() {
    Circle circle("red", 5.0);
    Rectangle rect("blue", 4.0, 6.0);
    
    printShapeInfo(circle);     // Calls Circle's methods
    printShapeInfo(rect);       // Calls Rectangle's methods
    
    // Dynamic polymorphism with pointers
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Circle>("green", 3.0));
    shapes.push_back(std::make_unique<Rectangle>("yellow", 2.0, 5.0));
    
    for (const auto& shape : shapes) {
        std::cout << shape->area() << "\n";
    }
}
```

### SOLID Principles

#### Single Responsibility Principle (SRP)
A class should have only one reason to change.

```cpp
// Bad: Multiple responsibilities
class User {
    void saveToDatabase() { /* ... */ }
    void sendEmail() { /* ... */ }
    void generateReport() { /* ... */ }
};

// Good: Separate responsibilities
class User {
    std::string name_;
    std::string email_;
    // ... user data only
};

class UserRepository {
    void save(const User& user) { /* ... */ }
};

class EmailService {
    void send(const std::string& to, const std::string& msg) { /* ... */ }
};
```

#### Open/Closed Principle (OCP)
Software entities should be open for extension but closed for modification.

```cpp
// Using polymorphism for extension
class PaymentProcessor {
public:
    virtual ~PaymentProcessor() = default;
    virtual void processPayment(double amount) = 0;
};

class CreditCardProcessor : public PaymentProcessor {
    void processPayment(double amount) override {
        // Credit card specific logic
    }
};

class PayPalProcessor : public PaymentProcessor {
    void processPayment(double amount) override {
        // PayPal specific logic
    }
};
```

#### Liskov Substitution Principle (LSP)
Derived classes must be substitutable for their base classes.

```cpp
class Bird {
public:
    virtual void move() = 0;
};

class Sparrow : public Bird {
    void move() override { /* fly */ }
};

// Penguin violates LSP if Bird has a fly() method
// Better design: separate flying and non-flying birds
class FlyingBird : public Bird {
    virtual void fly() = 0;
};

class NonFlyingBird : public Bird {
    virtual void walk() = 0;
};
```

#### Interface Segregation Principle (ISP)
Clients should not be forced to depend on interfaces they don't use.

```cpp
// Bad: Fat interface
class Worker {
    virtual void work() = 0;
    virtual void eat() = 0;
    virtual void sleep() = 0;
};

// Good: Segregated interfaces
class Workable {
    virtual void work() = 0;
};

class Feedable {
    virtual void eat() = 0;
};

class Human : public Workable, public Feedable {
    void work() override { /* ... */ }
    void eat() override { /* ... */ }
};

class Robot : public Workable {
    void work() override { /* ... */ }
    // Robot doesn't need to eat
};
```

#### Dependency Inversion Principle (DIP)
Depend on abstractions, not concretions.

```cpp
// Bad: High-level module depends on low-level module
class MySQLDatabase {
public:
    void save(const std::string& data) { /* ... */ }
};

class UserService {
    MySQLDatabase db_;  // Tight coupling
public:
    void createUser(const User& user) {
        db_.save(user.serialize());
    }
};

// Good: Both depend on abstraction
class Database {
public:
    virtual ~Database() = default;
    virtual void save(const std::string& data) = 0;
};

class MySQLDatabase : public Database {
    void save(const std::string& data) override { /* ... */ }
};

class PostgreSQLDatabase : public Database {
    void save(const std::string& data) override { /* ... */ }
};

class UserService {
    std::unique_ptr<Database> db_;  // Depends on abstraction
public:
    UserService(std::unique_ptr<Database> db) : db_(std::move(db)) {}
    
    void createUser(const User& user) {
        db_->save(user.serialize());
    }
};
```

---

## Designing C++ Systems

### Software Architecture Patterns

#### Layered Architecture

Organize system into horizontal layers, each providing services to the layer above.

```
┌─────────────────────────┐
│   Presentation Layer    │  (UI, API endpoints)
├─────────────────────────┤
│   Application Layer     │  (Business logic, use cases)
├─────────────────────────┤
│   Domain Layer          │  (Core domain models)
├─────────────────────────┤
│   Infrastructure Layer  │  (Database, external services)
└─────────────────────────┘
```

```cpp
// Domain layer
class Order {
    std::string id_;
    std::vector<OrderItem> items_;
    OrderStatus status_;
public:
    void addItem(const OrderItem& item);
    double calculateTotal() const;
    void setStatus(OrderStatus status);
};

// Application layer
class OrderService {
    OrderRepository& repository_;
    PaymentService& paymentService_;
public:
    OrderService(OrderRepository& repo, PaymentService& payment)
        : repository_(repo), paymentService_(payment) {}
    
    void placeOrder(const Order& order) {
        // Business logic
        if (order.calculateTotal() > 0) {
            repository_.save(order);
            paymentService_.processPayment(order);
        }
    }
};
```

#### Model-View-Controller (MVC)

Separates application into three interconnected components.

```cpp
// Model
class TodoModel {
    std::vector<std::string> todos_;
public:
    void addTodo(const std::string& todo) {
        todos_.push_back(todo);
    }
    
    const std::vector<std::string>& getTodos() const {
        return todos_;
    }
};

// View
class TodoView {
public:
    void display(const std::vector<std::string>& todos) {
        for (size_t i = 0; i < todos.size(); ++i) {
            std::cout << (i + 1) << ". " << todos[i] << "\n";
        }
    }
};

// Controller
class TodoController {
    TodoModel& model_;
    TodoView& view_;
public:
    TodoController(TodoModel& model, TodoView& view)
        : model_(model), view_(view) {}
    
    void addTodo(const std::string& todo) {
        model_.addTodo(todo);
        view_.display(model_.getTodos());
    }
};
```

### Design Patterns

#### Creational Patterns

**Singleton Pattern**: Ensure a class has only one instance.

```cpp
class Logger {
private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
public:
    static Logger& getInstance() {
        static Logger instance;  // Thread-safe in C++11+
        return instance;
    }
    
    void log(const std::string& message) {
        std::cout << "[LOG] " << message << "\n";
    }
};

// Usage
Logger::getInstance().log("Application started");
```

**Factory Pattern**: Create objects without specifying exact classes.

```cpp
class Document {
public:
    virtual ~Document() = default;
    virtual void open() = 0;
};

class PDFDocument : public Document {
    void open() override { std::cout << "Opening PDF\n"; }
};

class WordDocument : public Document {
    void open() override { std::cout << "Opening Word doc\n"; }
};

class DocumentFactory {
public:
    static std::unique_ptr<Document> createDocument(const std::string& type) {
        if (type == "pdf") {
            return std::make_unique<PDFDocument>();
        } else if (type == "word") {
            return std::make_unique<WordDocument>();
        }
        return nullptr;
    }
};
```

**Builder Pattern**: Construct complex objects step by step.

```cpp
class HttpRequest {
    std::string method_;
    std::string url_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    
public:
    class Builder {
        HttpRequest request_;
    public:
        Builder& setMethod(const std::string& method) {
            request_.method_ = method;
            return *this;
        }
        
        Builder& setUrl(const std::string& url) {
            request_.url_ = url;
            return *this;
        }
        
        Builder& addHeader(const std::string& key, const std::string& value) {
            request_.headers_[key] = value;
            return *this;
        }
        
        Builder& setBody(const std::string& body) {
            request_.body_ = body;
            return *this;
        }
        
        HttpRequest build() {
            return std::move(request_);
        }
    };
};

// Usage
auto request = HttpRequest::Builder()
    .setMethod("POST")
    .setUrl("https://api.example.com/users")
    .addHeader("Content-Type", "application/json")
    .setBody("{\"name\": \"John\"}")
    .build();
```

#### Structural Patterns

**Adapter Pattern**: Convert interface of a class into another interface.

```cpp
// Existing interface
class LegacyPrinter {
public:
    void printDocument(const char* text) {
        std::cout << "Legacy: " << text << "\n";
    }
};

// Target interface
class ModernPrinter {
public:
    virtual ~ModernPrinter() = default;
    virtual void print(const std::string& document) = 0;
};

// Adapter
class PrinterAdapter : public ModernPrinter {
    LegacyPrinter legacyPrinter_;
public:
    void print(const std::string& document) override {
        legacyPrinter_.printDocument(document.c_str());
    }
};
```

**Decorator Pattern**: Add responsibilities to objects dynamically.

```cpp
class Coffee {
public:
    virtual ~Coffee() = default;
    virtual double cost() const = 0;
    virtual std::string description() const = 0;
};

class SimpleCoffee : public Coffee {
public:
    double cost() const override { return 2.0; }
    std::string description() const override { return "Simple coffee"; }
};

class CoffeeDecorator : public Coffee {
protected:
    std::unique_ptr<Coffee> coffee_;
public:
    CoffeeDecorator(std::unique_ptr<Coffee> coffee)
        : coffee_(std::move(coffee)) {}
};

class MilkDecorator : public CoffeeDecorator {
public:
    using CoffeeDecorator::CoffeeDecorator;
    
    double cost() const override {
        return coffee_->cost() + 0.5;
    }
    
    std::string description() const override {
        return coffee_->description() + ", milk";
    }
};

class SugarDecorator : public CoffeeDecorator {
public:
    using CoffeeDecorator::CoffeeDecorator;
    
    double cost() const override {
        return coffee_->cost() + 0.2;
    }
    
    std::string description() const override {
        return coffee_->description() + ", sugar";
    }
};

// Usage
auto coffee = std::make_unique<SimpleCoffee>();
coffee = std::make_unique<MilkDecorator>(std::move(coffee));
coffee = std::make_unique<SugarDecorator>(std::move(coffee));
// Result: "Simple coffee, milk, sugar" costing 2.7
```

**Facade Pattern**: Provide simplified interface to complex subsystem.

```cpp
class CPU {
public:
    void freeze() { std::cout << "CPU frozen\n"; }
    void jump(long position) { std::cout << "CPU jump to " << position << "\n"; }
    void execute() { std::cout << "CPU executing\n"; }
};

class Memory {
public:
    void load(long position, const std::string& data) {
        std::cout << "Memory loaded at " << position << "\n";
    }
};

class HardDrive {
public:
    std::string read(long lba, int size) {
        return "boot_data";
    }
};

// Facade
class ComputerFacade {
    CPU cpu_;
    Memory memory_;
    HardDrive hardDrive_;
    
public:
    void start() {
        cpu_.freeze();
        memory_.load(0, hardDrive_.read(0, 1024));
        cpu_.jump(0);
        cpu_.execute();
    }
};

// Usage: Simple interface to complex boot process
ComputerFacade computer;
computer.start();
```

#### Behavioral Patterns

**Observer Pattern**: Define one-to-many dependency between objects.

```cpp
class Observer {
public:
    virtual ~Observer() = default;
    virtual void update(const std::string& message) = 0;
};

class Subject {
    std::vector<Observer*> observers_;
    std::string state_;
    
public:
    void attach(Observer* observer) {
        observers_.push_back(observer);
    }
    
    void detach(Observer* observer) {
        observers_.erase(
            std::remove(observers_.begin(), observers_.end(), observer),
            observers_.end()
        );
    }
    
    void notify() {
        for (auto* observer : observers_) {
            observer->update(state_);
        }
    }
    
    void setState(const std::string& state) {
        state_ = state;
        notify();
    }
};

class ConcreteObserver : public Observer {
    std::string name_;
public:
    ConcreteObserver(const std::string& name) : name_(name) {}
    
    void update(const std::string& message) override {
        std::cout << name_ << " received: " << message << "\n";
    }
};
```

**Strategy Pattern**: Define family of algorithms, make them interchangeable.

```cpp
class SortStrategy {
public:
    virtual ~SortStrategy() = default;
    virtual void sort(std::vector<int>& data) = 0;
};

class BubbleSort : public SortStrategy {
    void sort(std::vector<int>& data) override {
        // Bubble sort implementation
        std::cout << "Sorting using bubble sort\n";
    }
};

class QuickSort : public SortStrategy {
    void sort(std::vector<int>& data) override {
        // Quick sort implementation
        std::cout << "Sorting using quick sort\n";
    }
};

class Sorter {
    std::unique_ptr<SortStrategy> strategy_;
public:
    void setStrategy(std::unique_ptr<SortStrategy> strategy) {
        strategy_ = std::move(strategy);
    }
    
    void sort(std::vector<int>& data) {
        if (strategy_) {
            strategy_->sort(data);
        }
    }
};
```

**Command Pattern**: Encapsulate request as an object.

```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
};

class Light {
public:
    void on() { std::cout << "Light is on\n"; }
    void off() { std::cout << "Light is off\n"; }
};

class LightOnCommand : public Command {
    Light& light_;
public:
    LightOnCommand(Light& light) : light_(light) {}
    void execute() override { light_.on(); }
    void undo() override { light_.off(); }
};

class RemoteControl {
    std::vector<std::unique_ptr<Command>> history_;
public:
    void executeCommand(std::unique_ptr<Command> cmd) {
        cmd->execute();
        history_.push_back(std::move(cmd));
    }
    
    void undoLast() {
        if (!history_.empty()) {
            history_.back()->undo();
            history_.pop_back();
        }
    }
};
```

### Modularization and Abstraction

#### Header and Implementation Separation

```cpp
// math_utils.hpp
#ifndef MATH_UTILS_HPP
#define MATH_UTILS_HPP

namespace math {
    double calculateCircleArea(double radius);
    double calculateRectangleArea(double width, double height);
}

#endif

// math_utils.cpp
#include "math_utils.hpp"
#include <cmath>

namespace math {
    double calculateCircleArea(double radius) {
        return M_PI * radius * radius;
    }
    
    double calculateRectangleArea(double width, double height) {
        return width * height;
    }
}
```

#### Namespaces

```cpp
namespace company {
    namespace project {
        namespace utilities {
            class StringHelper {
                // ...
            };
        }
    }
}

// C++17 nested namespace
namespace company::project::utilities {
    class StringHelper {
        // ...
    };
}

// Usage
using company::project::utilities::StringHelper;
```

#### Modules (C++20)

```cpp
// math.ixx (module interface)
export module math;

export namespace math {
    double add(double a, double b) {
        return a + b;
    }
    
    double multiply(double a, double b) {
        return a * b;
    }
}

// main.cpp
import math;
int main() {
    double result = math::add(5.0, 3.0);
}
```

---

## Best Practices in C++ Development

### Modern C++ Standards

#### C++11 Features

- **Auto type deduction**: `auto x = 42;`
- **Range-based for loops**: `for (const auto& item : container)`
- **Lambda expressions**: `[](int x) { return x * 2; }`
- **Smart pointers**: `std::unique_ptr`, `std::shared_ptr`
- **Move semantics**: `std::move()`, rvalue references `&&`
- **nullptr**: Type-safe null pointer
- **constexpr**: Compile-time evaluation
- **Uniform initialization**: `std::vector<int> v{1, 2, 3};`

```cpp
// Lambda example
std::vector<int> numbers = {1, 2, 3, 4, 5};
std::for_each(numbers.begin(), numbers.end(), [](int n) {
    std::cout << n * n << " ";
});

// Move semantics
std::vector<int> createLargeVector() {
    std::vector<int> temp(1000000);
    return temp;  // Move, not copy
}
```

#### C++14 Features

- **Generic lambdas**: `[](auto x) { return x * 2; }`
- **Return type deduction**: `auto func() { return 42; }`
- **Binary literals**: `0b1010`
- **Digit separators**: `1'000'000`
- **`std::make_unique`**: Create unique pointers

```cpp
auto lambda = [](auto x, auto y) {
    return x + y;
};
int sum = lambda(5, 10);
double fsum = lambda(3.5, 2.7);
```

#### C++17 Features

- **Structured bindings**: `auto [x, y] = std::make_pair(1, 2);`
- **`if` with initializer**: `if (auto it = map.find(key); it != map.end())`
- **`std::optional`**: Represent optional values
- **`std::variant`**: Type-safe union
- **`std::filesystem`**: File system operations
- **Fold expressions**: `(args + ...)`

```cpp
// Structured bindings
std::map<std::string, int> scores = {{"Alice", 90}, {"Bob", 85}};
for (const auto& [name, score] : scores) {
    std::cout << name << ": " << score << "\n";
}

// std::optional
std::optional<std::string> findUser(int id) {
    if (id == 1) return "Alice";
    return std::nullopt;
}

if (auto user = findUser(1); user.has_value()) {
    std::cout << "Found: " << *user << "\n";
}
```

#### C++20 Features

- **Concepts**: Constrain template parameters
- **Ranges**: Composable algorithms
- **Coroutines**: Async programming support
- **Modules**: Better alternative to headers
- **Three-way comparison**: `operator <=>`
- **`std::span`**: Non-owning view of contiguous sequence

```cpp
// Concepts
template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

template<Numeric T>
T add(T a, T b) {
    return a + b;
}

// Ranges
std::vector<int> numbers = {1, 2, 3, 4, 5};
auto result = numbers 
    | std::views::filter([](int n) { return n % 2 == 0; })
    | std::views::transform([](int n) { return n * n; });
```

### Smart Pointers and Memory Management

#### RAII (Resource Acquisition Is Initialization)

RAII is a fundamental C++ idiom where resource lifetime is tied to object lifetime.

```cpp
class FileHandle {
    FILE* file_;
public:
    FileHandle(const char* filename, const char* mode) {
        file_ = fopen(filename, mode);
        if (!file_) {
            throw std::runtime_error("Failed to open file");
        }
    }
    
    ~FileHandle() {
        if (file_) {
            fclose(file_);
        }
    }
    
    // Prevent copying
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    
    // Allow moving
    FileHandle(FileHandle&& other) noexcept : file_(other.file_) {
        other.file_ = nullptr;
    }
    
    FILE* get() { return file_; }
};

// Usage: File automatically closed when object goes out of scope
void processFile() {
    FileHandle file("data.txt", "r");
    // Use file.get()
    // Automatic cleanup, even if exception thrown
}
```

#### std::unique_ptr

Exclusive ownership of dynamically allocated object.

```cpp
// Create unique_ptr
std::unique_ptr<int> ptr1 = std::make_unique<int>(42);
std::unique_ptr<int[]> arr = std::make_unique<int[]>(10);

// Transfer ownership (move)
std::unique_ptr<int> ptr2 = std::move(ptr1);  // ptr1 is now nullptr

// Custom deleter
auto deleter = [](FILE* fp) { if (fp) fclose(fp); };
std::unique_ptr<FILE, decltype(deleter)> file(
    fopen("data.txt", "r"), deleter
);
```

#### std::shared_ptr

Shared ownership with reference counting.

```cpp
std::shared_ptr<int> ptr1 = std::make_shared<int>(42);
std::shared_ptr<int> ptr2 = ptr1;  // Reference count = 2

std::cout << "Use count: " << ptr1.use_count() << "\n";

// Object deleted when last shared_ptr goes out of scope
```

#### std::weak_ptr

Non-owning reference to shared_ptr, breaks circular references.

```cpp
class Node {
public:
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;  // Break circular reference
    int data;
};

std::shared_ptr<Node> node1 = std::make_shared<Node>();
std::shared_ptr<Node> node2 = std::make_shared<Node>();

node1->next = node2;
node2->prev = node1;  // Weak reference, no cycle
```

### Avoiding Memory Leaks

```cpp
// Bad: Manual memory management
void badFunction() {
    int* data = new int[1000];
    // ... code that might throw ...
    delete[] data;  // Might not execute if exception thrown
}

// Good: RAII with smart pointers
void goodFunction() {
    auto data = std::make_unique<int[]>(1000);
    // ... code that might throw ...
    // Automatic cleanup, exception-safe
}

// Good: Use containers
void bestFunction() {
    std::vector<int> data(1000);
    // Automatic memory management
}
```

### Rule of Zero/Three/Five

#### Rule of Zero

If you don't need to manage resources, don't define special member functions.

```cpp
class Person {
    std::string name_;
    int age_;
    std::vector<std::string> hobbies_;
public:
    Person(std::string name, int age)
        : name_(std::move(name)), age_(age) {}
    // Compiler-generated copy/move constructors and assignment operators
    // work correctly because all members manage themselves
};
```

#### Rule of Three

If you define destructor, copy constructor, or copy assignment, define all three.

```cpp
class Buffer {
    char* data_;
    size_t size_;
public:
    // Constructor
    Buffer(size_t size) : data_(new char[size]), size_(size) {}
    
    // Destructor
    ~Buffer() {
        delete[] data_;
    }
    
    // Copy constructor
    Buffer(const Buffer& other) : size_(other.size_) {
        data_ = new char[size_];
        std::copy(other.data_, other.data_ + size_, data_);
    }
    
    // Copy assignment
    Buffer& operator=(const Buffer& other) {
        if (this != &other) {
            delete[] data_;
            size_ = other.size_;
            data_ = new char[size_];
            std::copy(other.data_, other.data_ + size_, data_);
        }
        return *this;
    }
};
```

#### Rule of Five

In C++11+, also define move constructor and move assignment.

```cpp
class Buffer {
    char* data_;
    size_t size_;
public:
    // ... (constructor, destructor, copy operations from Rule of Three)
    
    // Move constructor
    Buffer(Buffer&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }
    
    // Move assignment
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
};
```

### Const Correctness

```cpp
class Vector3D {
    double x_, y_, z_;
public:
    Vector3D(double x, double y, double z) : x_(x), y_(y), z_(z) {}
    
    // Const member functions (don't modify state)
    double getX() const { return x_; }
    double magnitude() const {
        return std::sqrt(x_*x_ + y_*y_ + z_*z_);
    }
    
    // Non-const member functions (can modify state)
    void setX(double x) { x_ = x; }
    void normalize() {
        double mag = magnitude();
        x_ /= mag; y_ /= mag; z_ /= mag;
    }
    
    // Const and non-const overloads
    double& operator[](size_t index) {
        if (index == 0) return x_;
        if (index == 1) return y_;
        return z_;
    }
    
    const double& operator[](size_t index) const {
        if (index == 0) return x_;
        if (index == 1) return y_;
        return z_;
    }
};

// Const references in function parameters
double dotProduct(const Vector3D& v1, const Vector3D& v2) {
    return v1.getX() * v2.getX() /* + ... */;
}
```

### Exception Safety

```cpp
// Basic guarantee: No resource leaks, objects remain valid
// Strong guarantee: Operation succeeds or has no effect (atomic)
// No-throw guarantee: Operation never throws exceptions

class SafeContainer {
    std::vector<int> data_;
public:
    // Strong exception safety using copy-and-swap
    void addData(const std::vector<int>& newData) {
        std::vector<int> temp = data_;  // Copy
        temp.insert(temp.end(), newData.begin(), newData.end());
        data_.swap(temp);  // No-throw swap
    }
    
    // No-throw guarantee
    size_t size() const noexcept {
        return data_.size();
    }
    
    // Use noexcept when appropriate
    void clear() noexcept {
        data_.clear();
    }
};
```

---

## C++ Libraries and Frameworks

### Standard Template Library (STL)

#### Containers

```cpp
#include <vector>
#include <list>
#include <deque>
#include <set>
#include <map>
#include <unordered_map>
#include <stack>
#include <queue>

// Sequence containers
std::vector<int> vec = {1, 2, 3, 4, 5};
std::list<std::string> lst = {"a", "b", "c"};
std::deque<double> deq;

// Associative containers
std::set<int> uniqueNumbers = {1, 2, 3, 3, 4};  // {1, 2, 3, 4}
std::map<std::string, int> ages = {{"Alice", 30}, {"Bob", 25}};

// Unordered containers (hash tables)
std::unordered_map<std::string, std::string> phoneBook;
phoneBook["Alice"] = "555-1234";

// Container adapters
std::stack<int> stack;
std::queue<int> queue;
std::priority_queue<int> pq;
```

#### Algorithms

```cpp
#include <algorithm>
#include <numeric>

std::vector<int> numbers = {5, 2, 8, 1, 9};

// Sorting
std::sort(numbers.begin(), numbers.end());

// Searching
auto it = std::find(numbers.begin(), numbers.end(), 8);
bool found = std::binary_search(numbers.begin(), numbers.end(), 5);

// Transforming
std::vector<int> squares(numbers.size());
std::transform(numbers.begin(), numbers.end(), squares.begin(),
               [](int n) { return n * n; });

// Filtering
auto endIter = std::remove_if(numbers.begin(), numbers.end(),
                               [](int n) { return n % 2 == 0; });
numbers.erase(endIter, numbers.end());

// Accumulating
int sum = std::accumulate(numbers.begin(), numbers.end(), 0);

// Partitioning
std::partition(numbers.begin(), numbers.end(),
               [](int n) { return n < 5; });
```

#### Iterators

```cpp
std::vector<int> vec = {1, 2, 3, 4, 5};

// Iterator types
std::vector<int>::iterator it = vec.begin();
std::vector<int>::const_iterator cit = vec.cbegin();
std::vector<int>::reverse_iterator rit = vec.rbegin();

// Iterator operations
std::advance(it, 2);  // Move iterator forward
int dist = std::distance(vec.begin(), vec.end());

// Iterator-based loops
for (auto it = vec.begin(); it != vec.end(); ++it) {
    std::cout << *it << " ";
}
```

### Boost Libraries

Boost provides peer-reviewed, portable C++ libraries.

```cpp
// Boost.Filesystem (before C++17 had std::filesystem)
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

fs::path p = "/usr/local/bin";
if (fs::exists(p) && fs::is_directory(p)) {
    for (const auto& entry : fs::directory_iterator(p)) {
        std::cout << entry.path() << "\n";
    }
}

// Boost.Asio (networking)
#include <boost/asio.hpp>

boost::asio::io_context io;
boost::asio::steady_timer timer(io, std::chrono::seconds(5));
timer.async_wait([](boost::system::error_code ec) {
    if (!ec) std::cout << "Timer expired\n";
});
io.run();

// Boost.Program_options
#include <boost/program_options.hpp>
namespace po = boost::program_options;

po::options_description desc("Options");
desc.add_options()
    ("help", "Show help message")
    ("input", po::value<std::string>(), "Input file");

po::variables_map vm;
po::store(po::parse_command_line(argc, argv, desc), vm);
```

### Qt Framework

Comprehensive framework for GUI and application development.

```cpp
#include <QApplication>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    QWidget window;
    QVBoxLayout layout(&window);
    
    QLabel label("Hello, Qt!");
    QPushButton button("Click me");
    
    QObject::connect(&button, &QPushButton::clicked, [&]() {
        label.setText("Button clicked!");
    });
    
    layout.addWidget(&label);
    layout.addWidget(&button);
    
    window.show();
    return app.exec();
}
```

### Other Popular Libraries

- **Poco**: C++ libraries for network programming, XML, JSON
- **SFML**: Multimedia library for games and graphics
- **Eigen**: Template library for linear algebra
- **OpenCV**: Computer vision library
- **gRPC**: RPC framework
- **JSON for Modern C++ (nlohmann/json)**: JSON parsing
- **spdlog**: Fast logging library
- **Catch2/Google Test**: Testing frameworks
- **fmt**: Modern formatting library

---

## Project Structure and Build Systems

### Organizing Project Structure

```
project/
├── CMakeLists.txt          # Build configuration
├── README.md               # Project documentation
├── LICENSE                 # License file
├── .gitignore              # Git ignore rules
│
├── include/                # Public headers
│   └── myproject/
│       ├── api.hpp
│       └── core.hpp
│
├── src/                    # Implementation files
│   ├── api.cpp
│   ├── core.cpp
│   └── internal/           # Private headers
│       └── helpers.hpp
│
├── tests/                  # Unit tests
│   ├── test_api.cpp
│   └── test_core.cpp
│
├── examples/               # Example applications
│   └── example_usage.cpp
│
├── docs/                   # Documentation
│   └── api_reference.md
│
├── third_party/            # External dependencies
│   └── libfoo/
│
└── build/                  # Build artifacts (gitignored)
```

### CMake Build System

#### Basic CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyProject VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/api.cpp
    src/core.cpp
)

# Create library
add_library(myproject ${SOURCES})

# Create executable
add_executable(myapp src/main.cpp)
target_link_libraries(myapp myproject)

# Compiler flags
target_compile_options(myproject PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# Install targets
install(TARGETS myproject myapp
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)

install(DIRECTORY include/ DESTINATION include)
```

#### Modern CMake Features

```cmake
# Interface libraries for header-only libraries
add_library(myheaderlib INTERFACE)
target_include_directories(myheaderlib INTERFACE include)

# Imported targets from find_package
find_package(Boost 1.70 REQUIRED COMPONENTS filesystem system)
target_link_libraries(myapp Boost::filesystem Boost::system)

# Generator expressions
target_compile_definitions(myapp PRIVATE
    $<$<CONFIG:Debug>:DEBUG_MODE>
    $<$<CONFIG:Release>:RELEASE_MODE>
)

# Export targets for use by other projects
install(TARGETS myproject EXPORT MyProjectTargets)
install(EXPORT MyProjectTargets
        FILE MyProjectTargets.cmake
        NAMESPACE MyProject::
        DESTINATION lib/cmake/MyProject)
```

#### Build Commands

```bash
# Configure
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Install
cmake --install build --prefix /usr/local

# Run tests
cd build && ctest
```

### Makefile (Traditional Approach)

```makefile
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic
INCLUDES = -Iinclude
LDFLAGS = -lpthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
TARGET = $(BIN_DIR)/myapp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
```

---

## Testing and Debugging

### Unit Testing with Google Test

#### Setup

```cpp
// test_math.cpp
#include <gtest/gtest.h>
#include "math_utils.hpp"

TEST(MathTest, Addition) {
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(add(-1, 1), 0);
    EXPECT_EQ(add(0, 0), 0);
}

TEST(MathTest, Division) {
    EXPECT_DOUBLE_EQ(divide(10.0, 2.0), 5.0);
    EXPECT_THROW(divide(10.0, 0.0), std::invalid_argument);
}

TEST(MathTest, VectorOperations) {
    std::vector<int> v1 = {1, 2, 3};
    std::vector<int> v2 = {1, 2, 3};
    EXPECT_EQ(v1, v2);
    EXPECT_NE(v1, std::vector<int>{4, 5, 6});
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

#### Test Fixtures

```cpp
class DatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<Database>("test.db");
        db_->connect();
    }
    
    void TearDown() override {
        db_->disconnect();
        std::remove("test.db");
    }
    
    std::unique_ptr<Database> db_;
};

TEST_F(DatabaseTest, InsertAndRetrieve) {
    db_->insert("key1", "value1");
    EXPECT_EQ(db_->get("key1"), "value1");
}

TEST_F(DatabaseTest, DeleteRecord) {
    db_->insert("key1", "value1");
    db_->remove("key1");
    EXPECT_FALSE(db_->exists("key1"));
}
```

#### Parameterized Tests

```cpp
class PrimeNumberTest : public ::testing::TestWithParam<int> {};

TEST_P(PrimeNumberTest, IsPrime) {
    EXPECT_TRUE(isPrime(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    PrimeNumbers,
    PrimeNumberTest,
    ::testing::Values(2, 3, 5, 7, 11, 13)
);
```

### Debugging with GDB

```bash
# Compile with debug symbols
g++ -g -O0 program.cpp -o program

# Start GDB
gdb ./program

# GDB commands
(gdb) break main              # Set breakpoint at main
(gdb) break file.cpp:42       # Set breakpoint at line 42
(gdb) run arg1 arg2           # Run program with arguments
(gdb) next                    # Execute next line (step over)
(gdb) step                    # Step into function
(gdb) continue                # Continue execution
(gdb) print variable          # Print variable value
(gdb) backtrace               # Show call stack
(gdb) info locals             # Show local variables
(gdb) watch variable          # Set watchpoint on variable
(gdb) quit                    # Exit GDB
```

### Memory Debugging with Valgrind

```bash
# Check for memory leaks
valgrind --leak-check=full --show-leak-kinds=all ./program

# Memory profiling
valgrind --tool=massif ./program
ms_print massif.out.12345

# Cache profiling
valgrind --tool=cachegrind ./program
cg_annotate cachegrind.out.12345
```

### Address Sanitizer

```bash
# Compile with AddressSanitizer
g++ -fsanitize=address -g -O1 program.cpp -o program

# Run program (reports memory errors immediately)
./program
```

### Static Analysis

#### clang-tidy

```bash
# Run clang-tidy
clang-tidy file.cpp -- -std=c++17 -Iinclude

# .clang-tidy configuration file
Checks: '-*,
  bugprone-*,
  cppcoreguidelines-*,
  modernize-*,
  performance-*,
  readability-*'
```

#### cppcheck

```bash
# Run cppcheck
cppcheck --enable=all --std=c++17 --inconclusive src/

# Suppress specific warnings
cppcheck --suppress=unusedFunction src/
```

### Profiling

#### gprof

```bash
# Compile with profiling
g++ -pg program.cpp -o program

# Run program
./program

# Generate profile
gprof program gmon.out > analysis.txt
```

#### perf (Linux)

```bash
# Record performance data
perf record ./program

# View report
perf report

# Check CPU usage by function
perf top
```

---

## Version Control and Collaboration

### Git Best Practices

#### Repository Initialization

```bash
# Initialize repository
git init
git remote add origin https://github.com/user/project.git

# Clone repository
git clone https://github.com/user/project.git
cd project
```

#### .gitignore for C++ Projects

```gitignore
# Compiled Object files
*.o
*.obj
*.ko
*.elf

# Precompiled Headers
*.gch
*.pch

# Compiled Dynamic libraries
*.so
*.dylib
*.dll

# Compiled Static libraries
*.a
*.lib

# Executables
*.exe
*.out
*.app

# Build directories
build/
cmake-build-*/
.vs/
Debug/
Release/

# IDE files
.vscode/
.idea/
*.vcxproj.user
*.suo

# CMake
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
Makefile

# Test outputs
Testing/
*.log

# Package managers
vcpkg_installed/
conan.lock
```

#### Branching Strategy

```bash
# Feature branch workflow
git checkout -b feature/new-parser
# ... make changes ...
git add .
git commit -m "feat: implement new bencode parser"
git push origin feature/new-parser

# Create pull request on GitHub/GitLab

# After review, merge to main
git checkout main
git merge feature/new-parser
git push origin main

# Delete feature branch
git branch -d feature/new-parser
git push origin --delete feature/new-parser
```

#### Commit Messages

Follow conventional commits format:

```
<type>(<scope>): <subject>

<body>

<footer>
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `style`: Code style changes (formatting)
- `refactor`: Code refactoring
- `test`: Adding or updating tests
- `chore`: Build process or auxiliary tool changes

Examples:
```bash
git commit -m "feat(parser): add support for UTF-8 encoding"
git commit -m "fix(memory): resolve memory leak in Buffer destructor"
git commit -m "docs(api): update API documentation for NetworkClient"
```

#### Code Review

```bash
# Before requesting review
git diff main...feature/my-branch  # View all changes
git log main..feature/my-branch    # View commits

# Address review comments
git add modified_file.cpp
git commit -m "fix: address review comments"
git push origin feature/my-branch

# Rebase before merging
git checkout feature/my-branch
git rebase main
git push --force-with-lease origin feature/my-branch
```

### Collaboration Workflow

#### Pull Request Process

1. **Create feature branch**
2. **Make changes** with atomic commits
3. **Write tests** for new functionality
4. **Update documentation** as needed
5. **Run tests and linters** locally
6. **Push branch** and create pull request
7. **Address review feedback**
8. **Merge** after approval

#### Code Review Checklist

- [ ] Code follows project style guidelines
- [ ] All tests pass
- [ ] New code has test coverage
- [ ] Documentation is updated
- [ ] No memory leaks or resource leaks
- [ ] Error handling is appropriate
- [ ] Performance considerations addressed
- [ ] Security implications reviewed

---

## Documentation and Comments

### Inline Comments

```cpp
// Good: Explain why, not what
// Use binary search because the array is sorted and contains millions of elements
int index = binarySearch(data, target);

// Bad: Restating the obvious
// Increment i by 1
i++;

// Good: Explain complex logic
// Calculate the checksum using CRC32 algorithm with polynomial 0x04C11DB7
// This matches the IEEE 802.3 standard used in Ethernet
uint32_t checksum = crc32(data, length);
```

### Documentation Comments (Doxygen)

```cpp
/**
 * @file network_client.hpp
 * @brief Network client for HTTP/HTTPS communication
 * @author John Doe
 * @date 2024-01-15
 */

/**
 * @class NetworkClient
 * @brief A client for making HTTP requests
 * 
 * The NetworkClient class provides a simple interface for making
 * HTTP GET and POST requests. It supports both HTTP and HTTPS
 * protocols and handles connection pooling automatically.
 * 
 * Example usage:
 * @code
 * NetworkClient client("https://api.example.com");
 * auto response = client.get("/users/123");
 * if (response.status == 200) {
 *     std::cout << response.body;
 * }
 * @endcode
 */
class NetworkClient {
public:
    /**
     * @brief Constructs a NetworkClient with the given base URL
     * @param baseUrl The base URL for all requests
     * @throws std::invalid_argument if baseUrl is empty
     */
    explicit NetworkClient(const std::string& baseUrl);
    
    /**
     * @brief Performs an HTTP GET request
     * @param path The path relative to the base URL
     * @param headers Optional HTTP headers
     * @return Response object containing status code and body
     * @throws NetworkException if the request fails
     * 
     * This method is thread-safe and can be called from multiple
     * threads simultaneously.
     */
    Response get(const std::string& path,
                const Headers& headers = {});
    
    /**
     * @brief Performs an HTTP POST request
     * @param path The path relative to the base URL
     * @param body The request body
     * @param headers Optional HTTP headers
     * @return Response object containing status code and body
     * @throws NetworkException if the request fails
     */
    Response post(const std::string& path,
                 const std::string& body,
                 const Headers& headers = {});

private:
    std::string baseUrl_;      ///< Base URL for requests
    ConnectionPool pool_;      ///< Connection pool for reuse
    
    /**
     * @brief Internal helper to execute requests
     * @param request The HTTP request to execute
     * @return Response from the server
     */
    Response executeRequest(const Request& request);
};
```

### Doxygen Configuration

Create `Doxyfile`:

```
# Project information
PROJECT_NAME           = "My C++ Project"
PROJECT_NUMBER         = 1.0.0
PROJECT_BRIEF          = "A brief description"

# Input settings
INPUT                  = include src docs
RECURSIVE              = YES
FILE_PATTERNS          = *.hpp *.cpp *.md

# Output settings
OUTPUT_DIRECTORY       = docs/generated
GENERATE_HTML          = YES
GENERATE_LATEX         = NO

# Extraction settings
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = YES

# Warnings
WARN_IF_UNDOCUMENTED   = YES
WARN_IF_DOC_ERROR      = YES
```

Generate documentation:
```bash
doxygen Doxyfile
```

### README Documentation

```markdown
# Project Name

Brief description of the project.

## Features

- Feature 1
- Feature 2
- Feature 3

## Requirements

- CMake 3.16+
- C++17 compatible compiler
- Boost 1.70+ (optional)

## Building

```bash
cmake -B build -S .
cmake --build build
```

## Installation

```bash
cmake --install build --prefix /usr/local
```

## Usage

```cpp
#include <myproject/api.hpp>

int main() {
    MyProject::Client client;
    client.connect("localhost", 8080);
    auto result = client.query("SELECT * FROM users");
    return 0;
}
```

## API Documentation

See [API Reference](docs/api.md) for detailed documentation.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) for details.

## License

This project is licensed under the MIT License - see [LICENSE](LICENSE).
```

### API Documentation Best Practices

1. **Document public interfaces thoroughly**
2. **Include usage examples**
3. **Explain parameters and return values**
4. **Document exceptions that can be thrown**
5. **Note thread-safety guarantees**
6. **Explain preconditions and postconditions**
7. **Document performance characteristics**
8. **Keep documentation up to date with code**

---

## Conclusion

Becoming an expert C++ architect requires mastery of language features, design principles, architectural patterns, and best practices. This guide has covered:

- **Language fundamentals**: From basic syntax to advanced template programming
- **OOP principles**: Encapsulation, inheritance, polymorphism, and SOLID principles
- **Design patterns**: Creational, structural, and behavioral patterns for common problems
- **Modern C++**: Features from C++11 through C++20 that improve code quality
- **Memory management**: RAII, smart pointers, and avoiding leaks
- **Libraries**: STL, Boost, Qt, and other essential C++ libraries
- **Build systems**: CMake and other tools for managing projects
- **Testing and debugging**: Tools and techniques for ensuring code quality
- **Version control**: Git workflows for effective collaboration
- **Documentation**: Writing clear, maintainable documentation

Continue learning by:
- Reading quality C++ codebases (LLVM, Chromium, Qt)
- Following C++ blogs and conferences (CppCon, Meeting C++)
- Practicing with real-world projects
- Staying updated with C++ standards evolution
- Participating in code reviews
- Contributing to open-source C++ projects

Remember: Great architecture emerges from balancing trade-offs, understanding constraints, and continuously refining your design skills through practice and feedback.
