import pandas as pd
from sklearn.neural_network import MLPRegressor
from sklearn.model_selection import train_test_split

# Đọc dữ liệu
df = pd.read_csv('calibration_data.csv')
X = df[['raw_temp', 'raw_hum', 'raw_gas']].values
y = df[['ref_temp', 'ref_hum', 'ref_gas']].values

# Chia tập
X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)

# Huấn luyện MLP
mlp = MLPRegressor(hidden_layer_sizes=(6,), activation='relu', solver='adam', max_iter=500, random_state=42)
mlp.fit(X_train, y_train)
print("Train R²:", mlp.score(X_train, y_train))
print("Test R²:", mlp.score(X_test, y_test))

# Trích trọng số
W1 = mlp.coefs_[0]  # (3, 6)
b1 = mlp.intercepts_[0]  # (6,)
W2 = mlp.coefs_[1]  # (6, 3)
b2 = mlp.intercepts_[1]  # (3,)

# Ghi file header cho C
with open('mlp_weights.h', 'w') as f:
    f.write('#ifndef MLP_WEIGHTS_H\n#define MLP_WEIGHTS_H\n\n')
    f.write('static const float W1[3][6] = {\n')
    for i in range(3):
        f.write('    {')
        for j in range(6):
            f.write(f'{W1[i][j]:.10f}')
            if j < 5: f.write(', ')
        f.write('},\n')
    f.write('};\n\n')

    f.write('static const float b1[6] = {')
    for i in range(6):
        f.write(f'{b1[i]:.10f}')
        if i < 5: f.write(', ')
    f.write('};\n\n')

    f.write('static const float W2[6][3] = {\n')
    for i in range(6):
        f.write('    {')
        for j in range(3):
            f.write(f'{W2[i][j]:.10f}')
            if j < 2: f.write(', ')
        f.write('},\n')
    f.write('};\n\n')

    f.write('static const float b2[3] = {')
    for i in range(3):
        f.write(f'{b2[i]:.10f}')
        if i < 2: f.write(', ')
    f.write('};\n\n')
    f.write('#endif\n')
